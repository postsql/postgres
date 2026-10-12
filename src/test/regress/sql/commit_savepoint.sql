--
-- COMMIT SAVEPOINT tests
--

CREATE TABLE csp_parent_tab (id int PRIMARY KEY, val text);
CREATE TABLE csp_child_tab (id int PRIMARY KEY, val text);
CREATE TABLE csp_child_tab2 (id int PRIMARY KEY, val text);

-- 1. Basic COMMIT SAVEPOINT: savepoint commits durably even if parent rolls back
BEGIN;
INSERT INTO csp_parent_tab VALUES (1, 'parent uncommitted');
SAVEPOINT s1;
INSERT INTO csp_child_tab VALUES (10, 'committed via s1');
COMMIT SAVEPOINT s1;
-- Still inside outer transaction
INSERT INTO csp_parent_tab VALUES (2, 'parent after s1');
ROLLBACK;

SELECT * FROM csp_parent_tab ORDER BY id;
SELECT * FROM csp_child_tab ORDER BY id;

-- 2. Syntax variants: COMMIT TO SAVEPOINT, COMMIT TRANSACTION TO SAVEPOINT, COMMIT TO
BEGIN;
SAVEPOINT s1;
INSERT INTO csp_child_tab VALUES (20, 'commit to savepoint');
COMMIT TO SAVEPOINT s1;
SAVEPOINT s2;
INSERT INTO csp_child_tab2 VALUES (21, 'commit transaction to savepoint');
COMMIT TRANSACTION TO SAVEPOINT s2;
SAVEPOINT s3;
INSERT INTO csp_child_tab VALUES (22, 'commit to name');
COMMIT TO s3;
ROLLBACK;

SELECT * FROM csp_child_tab ORDER BY id;
SELECT * FROM csp_child_tab2 ORDER BY id;

-- 3. Nested savepoints:
-- 3a. Committing an ancestor savepoint s1 commits s1 and its deeper child s2 atomically
BEGIN;
INSERT INTO csp_parent_tab VALUES (3, 'parent before nested');
SAVEPOINT s1;
INSERT INTO csp_child_tab VALUES (30, 'in s1');
SAVEPOINT s2;
INSERT INTO csp_child_tab2 VALUES (31, 'in s2');
COMMIT SAVEPOINT s1;
ROLLBACK;

SELECT * FROM csp_parent_tab ORDER BY id;
SELECT * FROM csp_child_tab WHERE id >= 30 ORDER BY id;
SELECT * FROM csp_child_tab2 WHERE id >= 30 ORDER BY id;

-- 3b. Committing inner savepoint s2 while outer savepoint s1 rolls back
BEGIN;
SAVEPOINT s1;
INSERT INTO csp_parent_tab VALUES (4, 'in s1 rolled back');
SAVEPOINT s2;
INSERT INTO csp_child_tab VALUES (40, 'in s2 committed');
COMMIT SAVEPOINT s2;
ROLLBACK TO SAVEPOINT s1;
COMMIT;

SELECT * FROM csp_parent_tab ORDER BY id;
SELECT * FROM csp_child_tab WHERE id >= 40 ORDER BY id;

-- 3c. Rolling back inner savepoint s2 and then committing s1
BEGIN;
SAVEPOINT s1;
INSERT INTO csp_child_tab VALUES (50, 'in s1 before s2 rollback');
SAVEPOINT s2;
INSERT INTO csp_child_tab VALUES (51, 'in s2 rolled back');
ROLLBACK TO SAVEPOINT s2;
INSERT INTO csp_child_tab VALUES (52, 'in s1 after s2 rollback');
COMMIT SAVEPOINT s1;
ROLLBACK;

SELECT * FROM csp_child_tab WHERE id >= 50 ORDER BY id;

-- 4. Read-only savepoint (no XID assigned)
BEGIN;
SAVEPOINT s_ro;
SELECT count(*) FROM csp_child_tab;
COMMIT SAVEPOINT s_ro;
COMMIT;

-- 5. REPEATABLE READ parent transaction sees its own committed savepoint tuples
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) FROM csp_child_tab;
SAVEPOINT s_rr;
INSERT INTO csp_child_tab VALUES (60, 'committed in repeatable read savepoint');
UPDATE csp_child_tab SET val = 'updated in repeatable read savepoint' WHERE id = 10;
COMMIT SAVEPOINT s_rr;
-- Parent's repeatable read snapshot still sees the changes committed by s_rr
SELECT id, val FROM csp_child_tab WHERE id IN (10, 60) ORDER BY id;
ROLLBACK;
-- And they persist after parent rolls back
SELECT id, val FROM csp_child_tab WHERE id IN (10, 60) ORDER BY id;

-- 6. Interaction with BEGIN AUTONOMOUS
BEGIN;
SAVEPOINT s_outer;
BEGIN AUTONOMOUS;
-- Cannot commit outer savepoint across autonomous boundary
COMMIT SAVEPOINT s_outer;
ROLLBACK;
ROLLBACK;

BEGIN;
BEGIN AUTONOMOUS;
SAVEPOINT s_in_auto;
INSERT INTO csp_child_tab VALUES (70, 'savepoint inside autonomous');
COMMIT SAVEPOINT s_in_auto;
ROLLBACK; -- rollback autonomous xact
ROLLBACK; -- rollback outer xact
SELECT id, val FROM csp_child_tab WHERE id = 70;

-- 7. Non-conflicting session SET inside savepoint commits durably on COMMIT SAVEPOINT
SHOW work_mem;
BEGIN;
SAVEPOINT s_guc;
SET work_mem = '16MB';
SHOW work_mem;
COMMIT SAVEPOINT s_guc;
ROLLBACK;
SHOW work_mem;
RESET work_mem;

-- =========================================================================
-- Blocker checks
-- =========================================================================

-- Error: outside transaction block
COMMIT SAVEPOINT s1;

-- Error: non-existent savepoint
BEGIN;
COMMIT SAVEPOINT no_such_sp;
ROLLBACK;

-- Error: savepoint in aborted state
BEGIN;
SAVEPOINT s_err;
SELECT 1 / 0;
COMMIT SAVEPOINT s_err;
ROLLBACK;

-- Blocker 1: SERIALIZABLE isolation level
BEGIN ISOLATION LEVEL SERIALIZABLE;
SAVEPOINT s_ser;
INSERT INTO csp_child_tab VALUES (100, 'serializable');
COMMIT SAVEPOINT s_ser;
ROLLBACK;

-- Blocker 2: Exported snapshot
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT pg_export_snapshot() IS NOT NULL AS exported;
SAVEPOINT s_exp;
INSERT INTO csp_child_tab VALUES (101, 'exported');
COMMIT SAVEPOINT s_exp;
ROLLBACK;

-- Blocker 4: Uncommitted parent tuple dependency (UPDATE, DELETE, SELECT FOR UPDATE)
-- 4a. UPDATE of tuple inserted by uncommitted parent
BEGIN;
INSERT INTO csp_parent_tab VALUES (100, 'parent row');
SAVEPOINT s_dep_upd;
UPDATE csp_parent_tab SET val = 'modified by child' WHERE id = 100;
COMMIT SAVEPOINT s_dep_upd;
ROLLBACK TO SAVEPOINT s_dep_upd;
COMMIT;
-- Parent's original row is still intact and can be committed
SELECT * FROM csp_parent_tab WHERE id = 100;

-- 4b. DELETE of tuple updated by uncommitted parent
BEGIN;
UPDATE csp_parent_tab SET val = 'updated by parent' WHERE id = 100;
SAVEPOINT s_dep_del;
DELETE FROM csp_parent_tab WHERE id = 100;
COMMIT SAVEPOINT s_dep_del;
ROLLBACK;

-- 4c. SELECT FOR UPDATE of tuple inserted by uncommitted parent
BEGIN;
INSERT INTO csp_parent_tab VALUES (101, 'parent row 2');
SAVEPOINT s_dep_lock;
SELECT * FROM csp_parent_tab WHERE id = 101 FOR UPDATE;
COMMIT SAVEPOINT s_dep_lock;
ROLLBACK;

-- 4d. Dependency in nested savepoint s2 propagated when committing s1
BEGIN;
INSERT INTO csp_parent_tab VALUES (102, 'parent row 3');
SAVEPOINT s1;
SAVEPOINT s2;
UPDATE csp_parent_tab SET val = 'modified in s2' WHERE id = 102;
COMMIT SAVEPOINT s1;
ROLLBACK;

-- Blocker 5: Lock ownership conflict with parent & explicit LOCK TABLE
-- 5a. Savepoint acquires lock conflicting with lock held by parent on same table
BEGIN;
LOCK TABLE csp_parent_tab IN SHARE MODE;
SAVEPOINT s_lock_conflict;
INSERT INTO csp_parent_tab VALUES (103, 'same table conflicting lock');
COMMIT SAVEPOINT s_lock_conflict;
ROLLBACK;

-- 5b. Explicit LOCK TABLE inside savepoint
BEGIN;
SAVEPOINT s_explicit_lock;
LOCK TABLE csp_child_tab IN SHARE MODE;
COMMIT SAVEPOINT s_explicit_lock;
ROLLBACK;

-- Blocker 6: DDL / Catalog invalidations
BEGIN;
SAVEPOINT s_ddl;
CREATE TABLE csp_ddl_tab (x int);
COMMIT SAVEPOINT s_ddl;
ROLLBACK;

-- Blocker 8: Pending trigger events (deferred constraint trigger)
CREATE TABLE csp_trig_tab (id int PRIMARY KEY);
CREATE OR REPLACE FUNCTION csp_trig_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    RETURN NEW;
END;
$$;
CREATE CONSTRAINT TRIGGER csp_after_trig AFTER INSERT ON csp_trig_tab
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION csp_trig_fn();

BEGIN;
SAVEPOINT s_trig;
INSERT INTO csp_trig_tab VALUES (1);
COMMIT SAVEPOINT s_trig;
ROLLBACK;

DROP TABLE csp_trig_tab;
DROP FUNCTION csp_trig_fn();

-- Blocker 9: Portals / Cursors
BEGIN;
SAVEPOINT s_cursor;
DECLARE c_sp CURSOR FOR SELECT * FROM csp_child_tab;
COMMIT SAVEPOINT s_cursor;
ROLLBACK;

-- Blocker 10: LISTEN / UNLISTEN / NOTIFY
BEGIN;
SAVEPOINT s_notify;
NOTIFY csp_channel, 'hello';
COMMIT SAVEPOINT s_notify;
ROLLBACK;

BEGIN;
SAVEPOINT s_listen;
LISTEN csp_channel;
COMMIT SAVEPOINT s_listen;
ROLLBACK;

-- Blocker 11: GUC blockers (SET LOCAL or SET conflicting with parent GUC change)
BEGIN;
SAVEPOINT s_set_local;
SET LOCAL work_mem = '16MB';
COMMIT SAVEPOINT s_set_local;
ROLLBACK;

BEGIN;
SET work_mem = '8MB';
SAVEPOINT s_set_conflict;
SET work_mem = '16MB';
COMMIT SAVEPOINT s_set_conflict;
ROLLBACK;

-- Blocker 15: Transaction-level advisory locks
BEGIN;
SAVEPOINT s_adv;
SELECT pg_advisory_xact_lock(987654);
COMMIT SAVEPOINT s_adv;
ROLLBACK;

-- Blocker 16: Large Objects
SELECT lo_create(424242);
BEGIN;
SAVEPOINT s_lo;
SELECT lo_open(424242, 131072) >= 0 AS opened;
COMMIT SAVEPOINT s_lo;
ROLLBACK;
SELECT lo_unlink(424242);

-- Blocker 6b: Uncommitted parent DDL / catalog changes before COMMIT SAVEPOINT
BEGIN;
CREATE SEQUENCE csp_uncommitted_seq;
SAVEPOINT s_parent_ddl;
INSERT INTO csp_child_tab VALUES (nextval('csp_uncommitted_seq')::int, 'seq from uncommitted parent');
COMMIT SAVEPOINT s_parent_ddl;
ROLLBACK;

-- 11. Nested savepoint rollup visibility in REPEATABLE READ
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) >= 0 AS snap_established FROM csp_child_tab;
SAVEPOINT sp_rr_outer;
INSERT INTO csp_child_tab VALUES (201, 'from sp_rr_outer');
SAVEPOINT sp_rr_inner;
INSERT INTO csp_child_tab VALUES (202, 'from sp_rr_inner');
COMMIT SAVEPOINT sp_rr_outer;
-- Both outer and inner savepoint rows must remain visible in REPEATABLE READ
SELECT id, val FROM csp_child_tab WHERE id IN (201, 202) ORDER BY id;
ROLLBACK;
SELECT id, val FROM csp_child_tab WHERE id IN (201, 202) ORDER BY id;

-- 12. Parallel query visibility in REPEATABLE READ after COMMIT SAVEPOINT
CREATE TABLE csp_parallel_tab (id int, pad text);
INSERT INTO csp_parallel_tab SELECT g, repeat('x', 100) FROM generate_series(1, 500) g;
ANALYZE csp_parallel_tab;

BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS before_sp FROM csp_parallel_tab;
SAVEPOINT sp_par;
INSERT INTO csp_parallel_tab SELECT g, repeat('y', 100) FROM generate_series(501, 700) g;
COMMIT SAVEPOINT sp_par;

SET LOCAL max_parallel_workers_per_gather = 2;
SET LOCAL parallel_setup_cost = 0;
SET LOCAL parallel_tuple_cost = 0;
SET LOCAL min_parallel_table_scan_size = 0;
SET LOCAL parallel_leader_participation = off;
SELECT count(*) AS parallel_count_after_csp FROM csp_parallel_tab;
ROLLBACK;

DROP TABLE csp_parallel_tab;
DROP TABLE csp_parent_tab;
DROP TABLE csp_child_tab;
DROP TABLE csp_child_tab2;
