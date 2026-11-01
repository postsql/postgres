-- REPLICA IDENTITY ROWID: DDL, decoding and guards.
-- This runs with wal_level = logical (see logical.conf).
SET synchronous_commit = on;

CREATE TABLE rowid_t (id int, val text) WITH (autovacuum_enabled = off);
ALTER TABLE rowid_t REPLICA IDENTITY ROWID;
SELECT relreplident FROM pg_class WHERE oid = 'rowid_t'::regclass;
\d+ rowid_t

-- "rowid" is not a keyword
CREATE TABLE rowid_kw (rowid int);
ALTER TABLE rowid_kw REPLICA IDENTITY "rowid";
ALTER TABLE rowid_kw REPLICA IDENTITY tid;          -- error
DROP TABLE rowid_kw;

-- not supported for partitioned tables, but for their partitions
CREATE TABLE rowid_parted (id int) PARTITION BY RANGE (id);
CREATE TABLE rowid_part1 PARTITION OF rowid_parted FOR VALUES FROM (0) TO (10);
ALTER TABLE rowid_parted REPLICA IDENTITY ROWID;    -- error
ALTER TABLE rowid_part1 REPLICA IDENTITY ROWID;
DROP TABLE rowid_parted;

-- the virtual column name ".rowid" is reserved
CREATE TABLE rowid_named (id int, ".rowid" tid);
ALTER TABLE rowid_named REPLICA IDENTITY ROWID;     -- error
DROP TABLE rowid_named;
ALTER TABLE rowid_t ADD COLUMN ".rowid" tid;        -- error
ALTER TABLE rowid_t RENAME COLUMN val TO ".rowid";  -- error

-- only the owner can change the replica identity
CREATE ROLE regress_rowid_user;
SET ROLE regress_rowid_user;
ALTER TABLE rowid_t REPLICA IDENTITY DEFAULT;       -- error
RESET ROLE;

-- TIDs are passed to the output plugin for ROWID tables, even with
-- logical_decoding_expose_headers off; and not for other tables.
CREATE TABLE rowid_other (id int) WITH (autovacuum_enabled = off);
SELECT 'init' FROM pg_create_logical_replication_slot('rowid_slot', 'test_decoding');
SHOW logical_decoding_expose_headers;
INSERT INTO rowid_t VALUES (1, 'one'), (2, 'two');
UPDATE rowid_t SET val = 'one-upd' WHERE id = 1;
DELETE FROM rowid_t WHERE id = 2;
INSERT INTO rowid_other VALUES (1);
SELECT data FROM pg_logical_slot_get_changes('rowid_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');
SELECT ctid, * FROM rowid_t;

-- UPDATE and DELETE are allowed on a ROWID table that publishes them
CREATE PUBLICATION rowid_pub FOR TABLE rowid_t;
UPDATE rowid_t SET val = 'one-upd2' WHERE id = 1;
DELETE FROM rowid_t WHERE id = 99;
-- but not if a row filter is used, which cannot be evaluated without the
-- old row
CREATE PUBLICATION rowid_pub_rf FOR TABLE rowid_t WHERE (id > 0);
INSERT INTO rowid_t VALUES (3, 'three');            -- ok
UPDATE rowid_t SET val = 'x' WHERE id = 3;          -- error
DELETE FROM rowid_t WHERE id = 3;                   -- error
ALTER PUBLICATION rowid_pub_rf SET (publish = 'insert');
UPDATE rowid_t SET val = 'x' WHERE id = 3;          -- ok now
DELETE FROM rowid_t WHERE id = 3;                   -- ok now
DROP PUBLICATION rowid_pub, rowid_pub_rf;

-- Rewriting the table would assign new TIDs: refused
CREATE INDEX rowid_t_id ON rowid_t (id);
VACUUM FULL rowid_t;                                -- error
CLUSTER rowid_t USING rowid_t_id;                   -- error
REPACK rowid_t;                                     -- error
ALTER TABLE rowid_t ALTER COLUMN id TYPE bigint;    -- error
ALTER TABLE rowid_t ADD COLUMN extra int;           -- ok, no rewrite
ALTER TABLE rowid_t ADD COLUMN extra2 int DEFAULT random()::int; -- error
-- other tables are processed, ROWID tables skipped, when not named
VACUUM FULL rowid_other, rowid_t;                   -- error (named)
VACUUM FULL;                                        -- warning, skipped
-- the TIDs are unchanged
SELECT ctid, id, val FROM rowid_t;
-- after changing the replica identity, rewriting is allowed again
ALTER TABLE rowid_t REPLICA IDENTITY DEFAULT;
VACUUM FULL rowid_t;
ALTER TABLE rowid_t REPLICA IDENTITY ROWID;

-- the changes above are decoded with TIDs
SELECT data FROM pg_logical_slot_get_changes('rowid_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');

SELECT pg_drop_replication_slot('rowid_slot');

-- pgoutput sends the tuple identifier as the virtual key column ".rowid"
CREATE TABLE rowid_po (id int, val text) WITH (autovacuum_enabled = off);
ALTER TABLE rowid_po REPLICA IDENTITY ROWID;
CREATE PUBLICATION rowid_po_pub FOR TABLE rowid_po;
SELECT 'init' FROM pg_create_logical_replication_slot('rowid_po_slot', 'pgoutput');
INSERT INTO rowid_po VALUES (1, 'one');
UPDATE rowid_po SET val = 'one-upd';
DELETE FROM rowid_po;
-- message type; whether the message contains the column name ".rowid", and
-- the tuple identifiers (0,1) and (0,2) in text format
SELECT chr(get_byte(data, 0)) AS msg,
	   position('\x2e726f77696400'::bytea IN data) > 0 AS rowid_attr,
	   position(convert_to('(0,1)', 'UTF8') IN data) > 0 AS tid_0_1,
	   position(convert_to('(0,2)', 'UTF8') IN data) > 0 AS tid_0_2
FROM pg_logical_slot_get_binary_changes('rowid_po_slot', NULL, NULL,
	'proto_version', '4', 'publication_names', 'rowid_po_pub')
WHERE chr(get_byte(data, 0)) IN ('R', 'I', 'U', 'D');
-- binary mode sends the tuple identifier in binary
INSERT INTO rowid_po VALUES (2, 'two');
SELECT chr(get_byte(data, 0)) AS msg,
	   position('\x6200000006000000000003'::bytea IN data) > 0 AS binary_tid_0_3
FROM pg_logical_slot_get_binary_changes('rowid_po_slot', NULL, NULL,
	'proto_version', '4', 'publication_names', 'rowid_po_pub', 'binary', 'true')
WHERE chr(get_byte(data, 0)) = 'I';
SELECT pg_drop_replication_slot('rowid_po_slot');
DROP PUBLICATION rowid_po_pub;

-- a ROWID partition cannot be published via its partition root
CREATE TABLE rowid_root (id int) PARTITION BY RANGE (id);
CREATE TABLE rowid_leaf PARTITION OF rowid_root FOR VALUES FROM (0) TO (10);
ALTER TABLE rowid_leaf REPLICA IDENTITY ROWID;
CREATE PUBLICATION rowid_root_pub FOR TABLE rowid_root WITH (publish_via_partition_root = true);
SELECT 'init' FROM pg_create_logical_replication_slot('rowid_root_slot', 'pgoutput');
INSERT INTO rowid_root VALUES (1);
\set VERBOSITY terse
SELECT count(*) FROM pg_logical_slot_get_binary_changes('rowid_root_slot', NULL, NULL,
	'proto_version', '4', 'publication_names', 'rowid_root_pub');
\set VERBOSITY default
SELECT pg_drop_replication_slot('rowid_root_slot');
DROP PUBLICATION rowid_root_pub;
DROP TABLE rowid_root;

DROP TABLE rowid_t, rowid_other, rowid_po;
DROP ROLE regress_rowid_user;
