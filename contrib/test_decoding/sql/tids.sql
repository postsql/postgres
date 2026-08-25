-- Test logical decoding with TIDs and header metadata
SET synchronous_commit = on;

-- 1. Test GUC values
SHOW logical_decoding_expose_headers;
SET logical_decoding_expose_headers = 'invalid'; -- should fail
SET logical_decoding_expose_headers = 'none';
SHOW logical_decoding_expose_headers;
SET logical_decoding_expose_headers = 'tids';
SHOW logical_decoding_expose_headers;
SET logical_decoding_expose_headers = 'all';
SHOW logical_decoding_expose_headers;

-- 2. Setup slot
SELECT 'init' FROM pg_create_logical_replication_slot('tid_slot', 'test_decoding');

CREATE TABLE tid_test (id int PRIMARY KEY, val text);

-- 3. With GUC = none, include-tids should not expose any TIDs
SET logical_decoding_expose_headers = 'none';
INSERT INTO tid_test VALUES (1, 'one');
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- 4. With GUC = tids
SET logical_decoding_expose_headers = 'tids';

-- Single insert
INSERT INTO tid_test VALUES (2, 'two');
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- Multi insert
INSERT INTO tid_test VALUES (3, 'three'), (4, 'four');
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- Update (in-place / new page)
UPDATE tid_test SET val = 'two-updated' WHERE id = 2;
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- Delete
DELETE FROM tid_test WHERE id = 2;
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- 5. Delete on table without replica identity (no old tuple logged, but old TID is still present)
CREATE TABLE tid_nopk (val text);
ALTER TABLE tid_nopk REPLICA IDENTITY NOTHING;
INSERT INTO tid_nopk VALUES ('hello');
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

DELETE FROM tid_nopk WHERE val = 'hello';
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- 6. Speculative insert (ON CONFLICT)
CREATE TABLE tid_conflict (id int PRIMARY KEY, val text);
INSERT INTO tid_conflict VALUES (1, 'initial') ON CONFLICT (id) DO NOTHING;
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

INSERT INTO tid_conflict VALUES (1, 'duplicate') ON CONFLICT (id) DO UPDATE SET val = EXCLUDED.val;
SELECT data FROM pg_logical_slot_get_changes('tid_slot', NULL, NULL, 'include-xids', '0', 'include-tids', '1');

-- 7. REPLICA IDENTITY ROWID and COPY tests
CREATE TABLE tid_rowid (id int, val text);
ALTER TABLE tid_rowid REPLICA IDENTITY ROWID;
INSERT INTO tid_rowid VALUES (10, 'ten');
COPY tid_rowid (val, ".rowid") TO stdout;
COPY tid_rowid (val, ctid) TO stdout;

-- Test pgoutput with REPLICA IDENTITY ROWID
CREATE PUBLICATION pub_rowid FOR TABLE tid_rowid;
SELECT 'init' FROM pg_create_logical_replication_slot('pgout_slot', 'pgoutput');
INSERT INTO tid_rowid VALUES (20, 'twenty');
UPDATE tid_rowid SET val = 'twenty-upd' WHERE id = 20;
DELETE FROM tid_rowid WHERE id = 20;
SELECT count(*) > 0 AS got_changes FROM pg_logical_slot_get_binary_changes('pgout_slot', NULL, NULL, 'proto_version', '1', 'publication_names', 'pub_rowid');
SELECT pg_drop_replication_slot('pgout_slot');
DROP PUBLICATION pub_rowid;
DROP TABLE tid_rowid;

-- 8. Index-Only Primary Key on Subscriber tests
SHOW logical_replication_index_only_rowid;

-- 8a. Table with .rowid and index: .rowid is omitted from physical heap (reads as NULL) but indexed
CREATE TABLE target_rowid (id int, val text, ".rowid" tid);
CREATE UNIQUE INDEX target_rowid_idx ON target_rowid (".rowid");
INSERT INTO target_rowid VALUES (1, 'one', '(0,1)'::tid);
-- Sequential scan reads physical heap (omitted .rowid is NULL)
SELECT id, val, ".rowid" IS NULL AS rowid_omitted_from_heap FROM target_rowid;
-- Index scan uses index on .rowid to locate row
SET enable_seqscan = off;
SELECT id, val FROM target_rowid WHERE ".rowid" = '(0,1)'::tid;
RESET enable_seqscan;

-- 8b. UPDATE via index on .rowid
UPDATE target_rowid SET val = 'one-updated', ".rowid" = '(0,2)'::tid WHERE ".rowid" = '(0,1)'::tid;
SET enable_seqscan = off;
SELECT id, val FROM target_rowid WHERE ".rowid" = '(0,2)'::tid;
RESET enable_seqscan;

-- 8c. DELETE via index on .rowid
DELETE FROM target_rowid WHERE ".rowid" = '(0,2)'::tid;
SELECT count(*) FROM target_rowid;

-- 8d. Post-migration cleanup (drop index and column without rewrite)
INSERT INTO target_rowid VALUES (2, 'two', '(0,3)'::tid);
DROP INDEX target_rowid_idx;
ALTER TABLE target_rowid DROP COLUMN ".rowid";
SELECT * FROM target_rowid;
DROP TABLE target_rowid;

-- 8e. Table-level reloption WITH (index_only_rowid = false) disables omission
CREATE TABLE target_rowid_stored (id int, val text, ".rowid" tid) WITH (index_only_rowid = false);
CREATE UNIQUE INDEX target_rowid_stored_idx ON target_rowid_stored (".rowid");
INSERT INTO target_rowid_stored VALUES (1, 'one', '(0,1)'::tid);
SELECT id, val, ".rowid" FROM target_rowid_stored;
DROP TABLE target_rowid_stored;

-- 9. Cleanup
DROP TABLE tid_test;
DROP TABLE tid_nopk;
DROP TABLE tid_conflict;
SELECT pg_drop_replication_slot('tid_slot');
