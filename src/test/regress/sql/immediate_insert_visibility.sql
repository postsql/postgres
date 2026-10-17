--
-- Tests for immediate_global_insert_visibility
--

SHOW immediate_global_insert_visibility;

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
SET immediate_global_insert_visibility = 'imm_vis_tab';
RESET immediate_global_insert_visibility;
SELECT set_config('immediate_global_insert_visibility', 'imm_vis_tab', false);

-- Validation of relation names in check_hook
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'nonexistent_table_xyz';
ROLLBACK;

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_view';
ROLLBACK;

-- 1. Basic autonomous / immediate visibility insert surviving ROLLBACK
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
SHOW immediate_global_insert_visibility;

INSERT INTO imm_vis_tab VALUES (1, 'survived rollback', repeat('a', 50000));
INSERT INTO norm_vis_tab VALUES (1, 'rolled back');

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;
ROLLBACK;

-- After ROLLBACK, GUC is reset and imm_vis_tab row (including TOAST) is still live
-- even though raw t_xmin is from an aborted transaction (because HEAP_XMIN_FROZEN is set).
SHOW immediate_global_insert_visibility;
SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;
SELECT * FROM norm_vis_tab;

-- Verify VACUUM FREEZE does not corrupt or remove the TOASTed row from aborted xact
VACUUM FREEZE imm_vis_tab;
SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status, length(large_val) FROM imm_vis_tab;

-- 2. Unique constraint violation in ExecInsertIndexTuples() must NOT freeze the tuple
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (1, 'duplicate key should not freeze', NULL);
ROLLBACK;

SET enable_indexscan = off;
SET enable_bitmapscan = off;
SELECT id, val FROM imm_vis_tab ORDER BY id;
RESET enable_indexscan;
RESET enable_bitmapscan;

-- 3. Halloween problem check: INSERT INTO ... SELECT FROM same table
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
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
SET LOCAL immediate_global_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_crashed_1');
ROLLBACK;

SELECT gid, pg_xact_status(xmin::text::xid8) AS xmin_status FROM twophase_gid_log ORDER BY gid;

-- Case B: Normal 2PC completion -> GID inserted frozen, then deleted and committed
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_completed_2');
DELETE FROM twophase_gid_log WHERE gid = 'gid_completed_2';
COMMIT;

SELECT gid FROM twophase_gid_log ORDER BY gid;

-- Case C: GID inserted frozen, deleted, then transaction aborts -> GID still survives!
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'twophase_gid_log';
INSERT INTO twophase_gid_log VALUES ('gid_delete_aborted_3');
DELETE FROM twophase_gid_log WHERE gid = 'gid_delete_aborted_3';
ROLLBACK;

SELECT gid, pg_xact_status(xmin::text::xid8) AS xmin_status FROM twophase_gid_log ORDER BY gid;

-- 5. INSERT ... ON CONFLICT support
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (1, 'conflict', NULL) ON CONFLICT (id) DO NOTHING;
INSERT INTO imm_vis_tab VALUES (2, 'inserted via on conflict', NULL) ON CONFLICT (id) DO NOTHING;
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab ORDER BY id;

-- 6. COPY (multi-insert) support
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
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
CREATE TABLE imm_part_4 (id int PRIMARY KEY, val text);

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_part_tab';
INSERT INTO imm_part_tab VALUES (10, 'part 1'), (150, 'part 2');
-- Partition attached AFTER setting GUC on parent must also inherit immediate visibility
ALTER TABLE imm_part_tab ATTACH PARTITION imm_part_4 FOR VALUES FROM (300) TO (400);
INSERT INTO imm_part_tab VALUES (350, 'part 4 attached after GUC');
ROLLBACK;

SELECT tableoid::regclass, id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_part_tab ORDER BY id;
SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_part_4 ORDER BY id;
DROP TABLE imm_part_4;

-- Partition created AFTER setting GUC on parent must inherit immediate visibility across ROLLBACK TO SAVEPOINT
BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_part_tab';
CREATE TABLE imm_part_3 PARTITION OF imm_part_tab FOR VALUES FROM (200) TO (300);
SAVEPOINT sp_part;
INSERT INTO imm_part_tab VALUES (250, 'part 3 created after GUC');
ROLLBACK TO SAVEPOINT sp_part;
COMMIT;

SELECT tableoid::regclass, id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_part_tab ORDER BY id;

-- 8. Sync / async modifiers and boundary inputs
BEGIN;
SET LOCAL immediate_global_insert_visibility = ',';
ROLLBACK;

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab:bogus';
ROLLBACK;

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab:async';
INSERT INTO imm_vis_tab VALUES (25, 'async suffix row', NULL);
ROLLBACK;

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'async:imm_vis_tab, twophase_gid_log:sync';
INSERT INTO imm_vis_tab VALUES (26, 'async prefix row', NULL);
INSERT INTO twophase_gid_log VALUES ('gid_sync_override');
ROLLBACK;

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'async, imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (27, 'async list keyword row', NULL);
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status
FROM imm_vis_tab WHERE id IN (25, 26, 27) ORDER BY id;
SELECT gid, pg_xact_status(xmin::text::xid8) AS xmin_status
FROM twophase_gid_log WHERE gid = 'gid_sync_override';

BEGIN;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
SET LOCAL immediate_global_insert_visibility = '';
INSERT INTO imm_vis_tab VALUES (30, 'not frozen after clearing guc', NULL);
ROLLBACK;

SELECT id, val FROM imm_vis_tab WHERE id = 30;

-- 9. Unprivileged role security check (including SECURITY DEFINER protection)
CREATE ROLE regress_imm_vis_user;
CREATE FUNCTION secdef_temp_insert() RETURNS void
LANGUAGE plpgsql SECURITY DEFINER AS $$
BEGIN
    INSERT INTO imm_vis_tab VALUES (99, 'secdef uncommitted row', NULL);
    RAISE EXCEPTION 'secdef intentional rollback';
END;
$$;
GRANT EXECUTE ON FUNCTION secdef_temp_insert() TO regress_imm_vis_user;

-- Unprivileged role without INSERT privilege must be rejected by GUC check_hook,
-- preventing it from forcing a SECURITY DEFINER function's uncommitted insert to freeze
BEGIN;
SET LOCAL ROLE regress_imm_vis_user;
SAVEPOINT sp_priv;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
ROLLBACK TO SAVEPOINT sp_priv;
SELECT secdef_temp_insert();
ROLLBACK;

-- Schema without USAGE privilege must also be rejected by GUC check_hook
CREATE SCHEMA regress_priv_schema;
CREATE TABLE regress_priv_schema.secret_tab (id int);
GRANT INSERT ON regress_priv_schema.secret_tab TO regress_imm_vis_user;
BEGIN;
SET LOCAL ROLE regress_imm_vis_user;
SET LOCAL immediate_global_insert_visibility = 'regress_priv_schema.secret_tab';
ROLLBACK;
DROP TABLE regress_priv_schema.secret_tab;
DROP SCHEMA regress_priv_schema;

-- Granting INSERT on imm_vis_tab allows setting the GUC and inserting
GRANT INSERT ON imm_vis_tab TO regress_imm_vis_user;
BEGIN;
SET LOCAL ROLE regress_imm_vis_user;
SET LOCAL immediate_global_insert_visibility = 'imm_vis_tab';
INSERT INTO imm_vis_tab VALUES (40, 'granted unprivileged insert', NULL);
ROLLBACK;

SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_vis_tab WHERE id IN (40, 99) ORDER BY id;

DROP FUNCTION secdef_temp_insert();
REVOKE ALL ON imm_vis_tab FROM regress_imm_vis_user;
DROP ROLE regress_imm_vis_user;

DROP VIEW imm_vis_view;
DROP TABLE imm_vis_tab;
DROP TABLE norm_vis_tab;
DROP TABLE twophase_gid_log;
DROP TABLE imm_part_tab;
