-- Tuple identifiers (TIDs) in logical decoding.
--
-- Tables are created fresh with autovacuum disabled, so TIDs are
-- deterministic.
SET synchronous_commit = on;

SELECT 'init' FROM pg_create_logical_replication_slot('tids_slot', 'test_decoding');

CREATE TABLE tids_t (id int PRIMARY KEY, val text) WITH (autovacuum_enabled = off);
CREATE TABLE tids_nokey (val text) WITH (autovacuum_enabled = off);
ALTER TABLE tids_nokey REPLICA IDENTITY NOTHING;
SELECT count(*) > 0 FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL);

-- By default, TIDs are not passed to the output plugin.
SHOW logical_decoding_expose_headers;
INSERT INTO tids_t VALUES (1, 'one');
UPDATE tids_t SET val = 'one-upd' WHERE id = 1;
DELETE FROM tids_t WHERE id = 1;
SELECT data FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');

-- The setting is evaluated by the decoding session, not when the change
-- was made: the changes below are made with the setting off.
INSERT INTO tids_t VALUES (2, 'two');                    -- single insert
INSERT INTO tids_t VALUES (3, 'three'), (4, 'four');     -- multi-row insert
UPDATE tids_t SET val = 'two-upd' WHERE id = 2;          -- HOT update
UPDATE tids_t SET id = 20 WHERE id = 2;                  -- non-HOT update
DELETE FROM tids_t WHERE id = 3;
INSERT INTO tids_t VALUES (4, 'dup') ON CONFLICT (id) DO UPDATE SET val = 'four-upd';
INSERT INTO tids_t VALUES (5, 'five') ON CONFLICT (id) DO NOTHING;
INSERT INTO tids_nokey VALUES ('hello');
DELETE FROM tids_nokey;                                  -- no old tuple data logged
COPY tids_t FROM stdin;                                  -- heap_multi_insert
6	six
7	seven
\.
SET logical_decoding_expose_headers = on;
SELECT data FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');

-- The TIDs match the actual tuple locations
SELECT ctid, id FROM tids_t ORDER BY id;

-- Rows placed on new pages, and an UPDATE moving a row to another page
CREATE TABLE tids_wide (id int, pad text) WITH (autovacuum_enabled = off, fillfactor = 10);
SELECT count(*) > 0 FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL);
INSERT INTO tids_wide SELECT g, repeat('x', 500) FROM generate_series(1, 3) g;
UPDATE tids_wide SET pad = repeat('y', 500) WHERE id = 1;
SELECT data FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');
SELECT ctid, id FROM tids_wide ORDER BY id;

-- COPY into an empty table initializes the page (XLOG_HEAP_INIT_PAGE)
CREATE TABLE tids_copy (id int) WITH (autovacuum_enabled = off);
SELECT count(*) > 0 FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL);
COPY tids_copy FROM stdin;
1
2
3
\.
SELECT data FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1');
SELECT ctid, id FROM tids_copy ORDER BY id;

-- Spilled transactions keep their TIDs
SET logical_decoding_work_mem = '64kB';
CREATE TABLE tids_spill (id int) WITH (autovacuum_enabled = off);
SELECT count(*) > 0 FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL);
INSERT INTO tids_spill SELECT g FROM generate_series(1, 5000) g;
SELECT count(*) AS changes,
	   count(*) FILTER (WHERE data ~ 'new-tid: \(\d+,\d+\)') AS with_tid,
	   count(DISTINCT substring(data FROM 'new-tid: (\(\d+,\d+\))')) AS distinct_tids
FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL,
	'include-xids', '0', 'skip-empty-xacts', '1', 'include-tids', '1')
WHERE data LIKE 'table %';
SELECT count(DISTINCT ctid) FROM tids_spill;
RESET logical_decoding_work_mem;

-- Invalid option value
SELECT data FROM pg_logical_slot_get_changes('tids_slot', NULL, NULL, 'include-tids', 'maybe');

RESET logical_decoding_expose_headers;
DROP TABLE tids_t, tids_nokey, tids_wide, tids_copy, tids_spill;
SELECT pg_drop_replication_slot('tids_slot');
