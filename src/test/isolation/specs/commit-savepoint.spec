# COMMIT SAVEPOINT isolation test
#
# Verify that changes and locks inside a savepoint committed via
# COMMIT SAVEPOINT become immediately visible and unlocked to concurrent
# sessions before the outer transaction ends, while uncommitted outer
# transaction changes remain invisible and roll back independently.

setup
{
    CREATE TABLE csp_iso_outer (id int PRIMARY KEY, val text);
    CREATE TABLE csp_iso_inner (id int PRIMARY KEY, val text);
}

teardown
{
    DROP TABLE csp_iso_outer;
    DROP TABLE csp_iso_inner;
}

session s1
step s1_begin       { BEGIN; }
step s1_outer_ins   { INSERT INTO csp_iso_outer VALUES (1, 'outer uncommitted'); }
step s1_sp          { SAVEPOINT sp1; }
step s1_inner_ins   { INSERT INTO csp_iso_inner VALUES (10, 'savepoint committed'); }
step s1_sp_commit   { COMMIT SAVEPOINT sp1; }
step s1_rollback    { ROLLBACK; }

session s2
step s2_select      { SELECT id, val FROM csp_iso_outer ORDER BY id; SELECT id, val FROM csp_iso_inner ORDER BY id; }
step s2_update      { UPDATE csp_iso_inner SET val = 'updated by s2 while s1 open' WHERE id = 10; }

permutation
    s1_begin
    s1_outer_ins
    s1_sp
    s1_inner_ins
    s2_select
    s1_sp_commit
    s2_select
    s2_update
    s1_rollback
    s2_select
