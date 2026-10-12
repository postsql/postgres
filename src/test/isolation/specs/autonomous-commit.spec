# Multi-session isolation test for BEGIN AUTONOMOUS / COMMIT AUTONOMOUS

setup
{
  CREATE TABLE auto_iso (id int PRIMARY KEY, val text);
  INSERT INTO auto_iso VALUES (1, 'init-1'), (2, 'init-2');
}

teardown
{
  DROP TABLE auto_iso;
}

session s1
step s1_begin       { BEGIN; }
step s1_upd1        { UPDATE auto_iso SET val = 's1-outer' WHERE id = 1; }
step s1_auto_begin  { BEGIN AUTONOMOUS; }
step s1_auto_ins    { INSERT INTO auto_iso VALUES (3, 's1-autonomous'); }
step s1_auto_upd2   { UPDATE auto_iso SET val = 's1-auto-upd2' WHERE id = 2; }
step s1_auto_commit { COMMIT AUTONOMOUS; }
step s1_rollback    { ROLLBACK; }
step s1_commit      { COMMIT; }

session s2
step s2_sel         { SELECT id, val FROM auto_iso ORDER BY id; }
step s2_upd2        { UPDATE auto_iso SET val = 's2-upd2' WHERE id = 2; }
step s2_upd1        { UPDATE auto_iso SET val = 's2-upd1' WHERE id = 1; }

# 1. Autonomous commit is immediately visible to s2 and releases locks on id=2,
# while s1 still holds its outer transaction lock on id=1 and later rolls back.
permutation
  s1_begin
  s1_upd1
  s1_auto_begin
  s1_auto_ins
  s1_auto_upd2
  s2_sel
  s1_auto_commit
  s2_sel
  s2_upd2
  s1_rollback
  s2_sel

# 2. s2 blocks on id=1 locked by s1's outer transaction even after s1 commits
# an autonomous transaction, and unblocks only when s1 finishes its outer xact.
permutation
  s1_begin
  s1_upd1
  s1_auto_begin
  s1_auto_ins
  s1_auto_commit
  s2_upd1
  s1_commit
  s2_sel
