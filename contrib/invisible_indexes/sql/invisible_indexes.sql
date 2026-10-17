-- Test suite for invisible_indexes extension

CREATE EXTENSION invisible_indexes;

SELECT invisible_indexes.version();

-- Setup test table and indexes
CREATE TABLE test_tab (
    id int,
    val int,
    payload text
);

INSERT INTO test_tab
SELECT i, i % 100, 'payload_' || i
FROM generate_series(1, 1000) i;

CREATE INDEX idx_test_id ON test_tab (id);
CREATE INDEX idx_test_val ON test_tab (val);

VACUUM ANALYZE test_tab;

-- Encourage index scans over sequential scans
SET enable_seqscan = off;

-- 1. Default: Indexes are visible and used by optimizer
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 2. Test session-level excluded_indexes GUC
SET excluded_indexes = 'idx_test_id';
SHOW excluded_indexes;

-- idx_test_id is now invisible to the planner (Seq Scan chosen despite enable_seqscan=off)
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

-- idx_test_val is still visible
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 3. Test multiple excluded indexes with duplicates (prompt requirement: SET excluded_indexes=i1, i2, i2)
SET excluded_indexes = 'idx_test_id, idx_test_val, idx_test_val';
SHOW excluded_indexes;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 4. Test whitespace handling, case insensitivity, and quotes in GUC
SET excluded_indexes = '  IDX_TEST_ID  ,   idx_test_val  ';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

-- 4b. Test quoted schema and quoted identifier parsing
SET excluded_indexes = '"public"."idx_test_id"';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

SET excluded_indexes = 'public."idx_test_val"';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 5. Test RESET excluded_indexes restores index visibility
RESET excluded_indexes;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 6. Test SET LOCAL (transaction-scoped exclusion)
BEGIN;
SET LOCAL excluded_indexes = 'idx_test_id';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
COMMIT;

-- After commit, idx_test_id is visible again
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

-- 7. Test DML maintenance & unique constraint enforcement while invisible
CREATE UNIQUE INDEX idx_test_unique ON test_tab (id) WHERE id > 10000;
SET excluded_indexes = 'idx_test_unique';
-- Query planner ignores idx_test_unique and falls back to idx_test_id
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 10001;

-- If both are excluded, planner falls back to sequential scan
SET excluded_indexes = 'idx_test_unique, idx_test_id';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 10001;
SET excluded_indexes = 'idx_test_unique';

-- DML inserts into the table and updates the invisible index
INSERT INTO test_tab VALUES (10001, 1, 'first insert');

-- Unique constraint is STILL ENFORCED by the storage layer!
INSERT INTO test_tab VALUES (10001, 1, 'duplicate insert');

-- Clean up test unique index
RESET excluded_indexes;
DROP INDEX idx_test_unique;

-- 8. Test persistent index invisibility via COMMENT DDL
COMMENT ON INDEX idx_test_val IS 'invisible';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- Clear persistent invisibility comment
COMMENT ON INDEX idx_test_val IS NULL;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE val = 42;

-- 9. Test SQL functions set_invisible and is_invisible (text and regclass overloads)
SELECT invisible_indexes.set_invisible('idx_test_id', true);
SELECT invisible_indexes.is_invisible('idx_test_id');
SELECT invisible_indexes.is_invisible('idx_test_id'::regclass);
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

SELECT invisible_indexes.set_invisible('idx_test_id'::regclass, false);
SELECT invisible_indexes.is_invisible('idx_test_id');
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

-- 9b. Test ALTER INDEX syntactic analogues: alter_index_invisible / alter_index_visible
SELECT invisible_indexes.alter_index_invisible('idx_test_id');
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
SELECT invisible_indexes.alter_index_visible('idx_test_id'::regclass);
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

-- 9c. Test preserving existing user comments on index
COMMENT ON INDEX idx_test_id IS 'Important business index';
SELECT invisible_indexes.set_invisible('idx_test_id', true);
SELECT description FROM pg_description WHERE objoid = 'idx_test_id'::regclass::oid;
SELECT invisible_indexes.set_invisible('idx_test_id', false);
SELECT description FROM pg_description WHERE objoid = 'idx_test_id'::regclass::oid;
COMMENT ON INDEX idx_test_id IS NULL;

-- 10. Test optimizer override: invisible_indexes.use_invisible_indexes
SET excluded_indexes = 'idx_test_id';
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

SET invisible_indexes.use_invisible_indexes = on;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;

SET invisible_indexes.use_invisible_indexes = off;
RESET excluded_indexes;

-- 11. Test global enable/disable toggle
SET excluded_indexes = 'idx_test_id';
SET invisible_indexes.enabled = off;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
SET invisible_indexes.enabled = on;
EXPLAIN (COSTS OFF) SELECT * FROM test_tab WHERE id = 42;
RESET excluded_indexes;

-- 11b. Test partitioned table and partitioned index inheritance
CREATE TABLE part_parent (id int, val int) PARTITION BY RANGE (id);
CREATE TABLE part_1 PARTITION OF part_parent FOR VALUES FROM (1) TO (100);
CREATE TABLE part_2 PARTITION OF part_parent FOR VALUES FROM (100) TO (200);
INSERT INTO part_parent SELECT i, i FROM generate_series(1, 199) i;
CREATE INDEX idx_part_parent ON part_parent (id);
VACUUM ANALYZE part_parent;

-- Default: partition index is used for query on partitioned table
EXPLAIN (COSTS OFF) SELECT * FROM part_parent WHERE id = 50;

-- Exclude partitioned index by its parent index name
SET excluded_indexes = 'idx_part_parent';
EXPLAIN (COSTS OFF) SELECT * FROM part_parent WHERE id = 50;
RESET excluded_indexes;

-- Persist partitioned index invisibility via function
SELECT invisible_indexes.set_invisible('idx_part_parent', true);
EXPLAIN (COSTS OFF) SELECT * FROM part_parent WHERE id = 50;
SELECT invisible_indexes.set_invisible('idx_part_parent', false);
EXPLAIN (COSTS OFF) SELECT * FROM part_parent WHERE id = 50;

DROP TABLE part_parent CASCADE;

-- 12. Security testing: Unprivileged user permissions
CREATE ROLE regress_invisible_indexes_user;
GRANT USAGE ON SCHEMA invisible_indexes TO regress_invisible_indexes_user;

-- Test public execute revocation
SET ROLE regress_invisible_indexes_user;
SELECT invisible_indexes.set_invisible('idx_test_id', true);
SELECT invisible_indexes.alter_index_invisible('idx_test_id');

-- Test superuser verification inside C function
RESET ROLE;
GRANT EXECUTE ON FUNCTION invisible_indexes.set_invisible(text, boolean) TO regress_invisible_indexes_user;
SET ROLE regress_invisible_indexes_user;
SELECT invisible_indexes.set_invisible('idx_test_id', true);

RESET ROLE;
REVOKE EXECUTE ON FUNCTION invisible_indexes.set_invisible(text, boolean) FROM regress_invisible_indexes_user;
REVOKE USAGE ON SCHEMA invisible_indexes FROM regress_invisible_indexes_user;
DROP ROLE regress_invisible_indexes_user;

-- 13. Negative & boundary tests
SELECT invisible_indexes.is_invisible(NULL);
SELECT invisible_indexes.is_invisible('non_existent_index');
SELECT invisible_indexes.is_invisible('test_tab');
SELECT invisible_indexes.set_invisible(NULL, true);
SELECT invisible_indexes.set_invisible('test_tab', true); -- wrong object type (table, not index)

-- View inspection
SELECT schemaname, indexname, tablename, is_persisted_invisible, is_currently_invisible
FROM invisible_indexes.invisible_indexes
WHERE tablename = 'test_tab'
ORDER BY indexname;

-- Cleanup
DROP TABLE test_tab CASCADE;
DROP EXTENSION invisible_indexes;
