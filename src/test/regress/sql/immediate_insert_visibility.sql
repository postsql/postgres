--
-- Tests for immediate_insert_visibility and immediate_insert_wal_flush
--

SHOW immediate_insert_visibility;

CREATE TABLE imm_vis_tab (
    id int PRIMARY KEY,
    val text,
    large_val text
);

CREATE TABLE norm_vis_tab (
    id int PRIMARY KEY,
    val text
);

CREATE VIEW imm_vis_view AS SELECT * FROM imm_vis_tab;

-- Must only be set with SET LOCAL
SET immediate_insert_visibility = 'imm_vis_tab';
RESET immediate_insert_visibility;
SELECT set_config('immediate_insert_visibility', 'imm_vis_tab', false);

-- Validation of relation names in check_hook
BEGIN;
SET LOCAL immediate_insert_visibility = 'nonexistent_table_xyz';
ROLLBACK;

BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_view';
ROLLBACK;

-- 1. Basic autonomous / immediate visibility insert surviving ROLLBACK
BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
SHOW immediate_insert_visibility;

INSERT INTO imm_vis_tab VALUES (1, 'survived rollback', repeat('a', 50000));
INSERT INTO norm_vis_tab VALUES (1, 'rolled back');

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;
ROLLBACK;

-- After ROLLBACK, GUC is reset and imm_vis_tab row (including TOAST) is still live
-- even though raw t_xmin is from an aborted transaction (because HEAP_XMIN_FROZEN is set).
SHOW immediate_insert_visibility;
SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;
SELECT * FROM norm_vis_tab;

-- Verify VACUUM FREEZE does not corrupt or remove the TOASTed row from aborted xact
VACUUM FREEZE imm_vis_tab;
SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;

-- 2. Unique constraint violation in ExecInsertIndexTuples() must NOT freeze the tuple
BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (1, 'duplicate key should not freeze', NULL);
ROLLBACK;

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id, val FROM imm_vis_tab ORDER BY id;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- 3. Halloween problem check: INSERT INTO ... SELECT FROM same table
BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab SELECT id + 10, val || ' copy', NULL FROM imm_vis_tab;
SELECT id, val FROM imm_vis_tab ORDER BY id;
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab ORDER BY id;

-- 4. 2PC local prepare log pattern: INSERT frozen, then DELETE in same xact
CREATE TABLE twophase_gid_log (
    gid text PRIMARY KEY
);

-- Case A: Crash / abort before 2PC completes -> GID record survives
BEGIN;
SET LOCAL immediate_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_crashed_1');
ROLLBACK;

SELECT gid, pg_xact_status(xmin::text::xid8) AS xmin_status FROM twophase_gid_log ORDER BY gid;

-- Case B: Normal 2PC completion -> GID inserted frozen, then deleted and committed
BEGIN;
SET LOCAL immediate_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_completed_2');
DELETE FROM twophase_gid_log WHERE gid = 'gid_completed_2';
COMMIT;

SELECT gid FROM twophase_gid_log ORDER BY gid;

-- Case C: GID inserted frozen, deleted, then transaction aborts -> GID still survives!
BEGIN;
SET LOCAL immediate_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_delete_aborted_3');
DELETE FROM twophase_gid_log WHERE gid = 'gid_delete_aborted_3';
ROLLBACK;

SELECT gid, pg_xact_status(xmin::text::xid8) AS xmin_status FROM twophase_gid_log ORDER BY gid;

-- 5. INSERT ... ON CONFLICT support
BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (1, 'conflict', NULL) ON CONFLICT (id) DO NOTHING;
INSERT INTO imm_vis_tab VALUES (2, 'inserted via on conflict', NULL) ON CONFLICT (id) DO NOTHING;
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab ORDER BY id;

-- 6. COPY (multi-insert) support
BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
COPY imm_vis_tab (id, val, large_val) FROM STDIN;
20	copy row 20	\N
21	copy row 21	\N
\.
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab ORDER BY id;

-- 7. Partitioned table support
CREATE TABLE imm_part_tab (
    id int PRIMARY KEY,
    val text
) PARTITION BY RANGE (id);

CREATE TABLE imm_part_1 PARTITION OF imm_part_tab FOR VALUES FROM (1) TO (100);
CREATE TABLE imm_part_2 PARTITION OF imm_part_tab FOR VALUES FROM (100) TO (200);

BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_part_tab';
INSERT INTO imm_part_tab VALUES (10, 'part 1'), (150, 'part 2');
ROLLBACK;

SELECT tableoid::regclass, id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_part_tab ORDER BY id;

-- 8. Boundary inputs and clearing mid-transaction
BEGIN;
SET LOCAL immediate_insert_visibility = ',';
ROLLBACK;

BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
SET LOCAL immediate_insert_visibility = '';
INSERT INTO imm_vis_tab VALUES (30, 'not frozen after clearing guc', NULL);
ROLLBACK;

SELECT id, val FROM imm_vis_tab WHERE id = 30;

-- 9. Unprivileged role security check
CREATE ROLE regress_imm_vis_user;
BEGIN;
SET LOCAL ROLE regress_imm_vis_user;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (40, 'unprivileged insert must fail', NULL);
ROLLBACK;

GRANT INSERT ON imm_vis_tab TO regress_imm_vis_user;
BEGIN;
SET LOCAL ROLE regress_imm_vis_user;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (40, 'granted unprivileged insert', NULL);
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab WHERE id = 40;

REVOKE ALL ON imm_vis_tab FROM regress_imm_vis_user;
DROP ROLE regress_imm_vis_user;

-- 10. immediate_insert_wal_flush parameter checks
SHOW immediate_insert_wal_flush;
SET immediate_insert_wal_flush = off;
SHOW immediate_insert_wal_flush;
RESET immediate_insert_wal_flush;
SHOW immediate_insert_wal_flush;

BEGIN;
SET LOCAL immediate_insert_visibility = 'imm_vis_tab';
SET LOCAL immediate_insert_wal_flush = off;
INSERT INTO imm_vis_tab VALUES (50, 'inserted with wal flush off', NULL);
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab WHERE id = 50;

DROP VIEW imm_vis_view;
DROP TABLE imm_vis_tab;
DROP TABLE norm_vis_tab;
DROP TABLE twophase_gid_log;
DROP TABLE imm_part_tab;
