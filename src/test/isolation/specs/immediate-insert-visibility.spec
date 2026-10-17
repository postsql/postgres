# Immediate global insert visibility cross-session isolation test
#
# Verifies that rows inserted into a table listed in
# immediate_global_insert_visibility are immediately visible to concurrent
# sessions (even under a REPEATABLE READ snapshot taken before the insert)
# while the inserting transaction is still in progress, and remain visible
# if the inserting transaction rolls back.
# Also verifies the 2PC local GID log lifecycle across sessions.

setup
{
 CREATE TABLE imm_iso_tab (id int PRIMARY KEY, val text);
 CREATE TABLE norm_iso_tab (id int PRIMARY KEY, val text);
}

teardown
{
 DROP TABLE imm_iso_tab, norm_iso_tab;
}

session s1
step s1_begin		{ BEGIN; SET LOCAL immediate_global_insert_visibility = 'imm_iso_tab'; }
step s1_insert		{ INSERT INTO imm_iso_tab VALUES (1, 'imm_row_1'); INSERT INTO norm_iso_tab VALUES (1, 'norm_row_1'); }
step s1_insert_2pc	{ INSERT INTO imm_iso_tab VALUES (2, 'gid_2pc'); }
step s1_delete_2pc	{ DELETE FROM imm_iso_tab WHERE id = 2; }
step s1_commit		{ COMMIT; }
step s1_rollback	{ ROLLBACK; }

session s2
step s2_begin_rr	{ BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM imm_iso_tab; }
step s2_select		{ SELECT id, val, pg_xact_status(xmin::text::xid8) AS xmin_status FROM imm_iso_tab ORDER BY id; SELECT id, val FROM norm_iso_tab ORDER BY id; }
step s2_commit		{ COMMIT; }

# 1. Concurrent session with pre-existing REPEATABLE READ snapshot sees
#    uncommitted immediate-visibility insert immediately, and still sees it
#    after s1 rolls back.
permutation s2_begin_rr s1_begin s1_insert s2_select s1_rollback s2_select s2_commit

# 2. 2PC GID log pattern: s1 inserts frozen row then deletes it in same xact;
#    s2 still sees the row while s1's DELETE is uncommitted, and sees it
#    removed once s1 commits.
permutation s1_begin s1_insert_2pc s2_select s1_delete_2pc s2_select s1_commit s2_select

# 3. 2PC GID log pattern with abort after DELETE: row remains visible to s2.
permutation s1_begin s1_insert_2pc s1_delete_2pc s2_select s1_rollback s2_select
