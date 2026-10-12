# Tests for automatic retry of autocommit-mode statements encountering
# serialization failures, deadlocks, and (optionally) DDL lock/statement
# timeouts.

setup
{
  CREATE TABLE retry_tbl (id int PRIMARY KEY, val int NOT NULL);
  INSERT INTO retry_tbl VALUES (1, 10), (2, 20);

  CREATE SEQUENCE retry_attempt_seq;

  CREATE FUNCTION fail_n_times(target_failures int, err_sqlstate text)
  RETURNS int LANGUAGE plpgsql AS $$
  DECLARE
    cur_attempt int;
  BEGIN
    cur_attempt := nextval('retry_attempt_seq');
    IF cur_attempt <= target_failures THEN
      RAISE EXCEPTION 'simulated transient failure on attempt %', cur_attempt
        USING ERRCODE = err_sqlstate;
    END IF;
    RETURN cur_attempt;
  END;
  $$;

  CREATE PROCEDURE retry_proc_simple()
  LANGUAGE plpgsql AS $$
  BEGIN
    UPDATE retry_tbl SET val = val + 1 WHERE id = 2;
  END;
  $$;

  CREATE PROCEDURE retry_proc_with_commit()
  LANGUAGE plpgsql AS $$
  BEGIN
    UPDATE retry_tbl SET val = val + 1 WHERE id = 1;
    COMMIT;
    SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
    UPDATE retry_tbl SET val = val + 1 WHERE id = 2;
  END;
  $$;
}

teardown
{
  DROP PROCEDURE retry_proc_with_commit();
  DROP PROCEDURE retry_proc_simple();
  DROP FUNCTION fail_n_times(int, text);
  DROP SEQUENCE retry_attempt_seq;
  DROP TABLE retry_tbl;
}

session s1
step s1_begin       { BEGIN; }
step s1_upd2        { UPDATE retry_tbl SET val = val + 100 WHERE id = 2; }
step s1_commit      { COMMIT; }

session s2
setup
{
  SET default_transaction_isolation = 'repeatable read';
  SET autocommit_retry_backoff_base_ms = 1;
  SET autocommit_retry_backoff_max_ms = 5;
}
step s2_retries_0   { SET max_autocommit_retries = 0; }
step s2_retries_2   { SET max_autocommit_retries = 2; }
step s2_retries_3   { SET max_autocommit_retries = 3; }
step s2_upd_ret     { UPDATE retry_tbl SET val = val + 1 RETURNING id, val; }
step s2_begin       { BEGIN; }
step s2_rollback    { ROLLBACK; }
step s2_multistmt   { SELECT 1; UPDATE retry_tbl SET val = val + 1 WHERE id = 2; }
step s2_call_simple { CALL retry_proc_simple(); }
step s2_call_commit { CALL retry_proc_with_commit(); }
step s2_check_tbl   { SELECT id, val FROM retry_tbl ORDER BY id; }
step s2_reset_seq
{
  SET log_autocommit_retries = on;
  SELECT setval('retry_attempt_seq', 1, false);
}
step s2_test_40001  { SELECT fail_n_times(2, '40001') AS attempts_40001; }
step s2_test_40p01  { SELECT fail_n_times(2, '40P01') AS attempts_40p01; }
step s2_exhaust     { SELECT fail_n_times(3, '40001') AS should_fail; }
step s2_check_seq   { SELECT last_value FROM retry_attempt_seq; }
step s2_ddl_setup
{
  CREATE FUNCTION ddl_fail_once() RETURNS event_trigger LANGUAGE plpgsql AS $$
  BEGIN
    IF nextval('retry_attempt_seq') <= 1 THEN
      RAISE EXCEPTION 'simulated DDL lock timeout' USING ERRCODE = '55P03';
    END IF;
  END;
  $$;
  CREATE EVENT TRIGGER ddl_retry_trig ON ddl_command_start
    WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION ddl_fail_once();
  SET enable_ddl_autocommit_retry = off;
  SELECT setval('retry_attempt_seq', 1, false);
}
step s2_ddl_alter   { ALTER TABLE retry_tbl ADD COLUMN extra int DEFAULT 42; }
step s2_ddl_enable
{
  SET enable_ddl_autocommit_retry = on;
  SELECT setval('retry_attempt_seq', 1, false);
}
step s2_ddl_stmttimeout_setup
{
  SELECT last_value AS ddl_lock_attempts FROM retry_attempt_seq;
  DROP EVENT TRIGGER ddl_retry_trig;
  DROP FUNCTION ddl_fail_once();

  CREATE FUNCTION ddl_sleep_once() RETURNS event_trigger LANGUAGE plpgsql AS $$
  BEGIN
    IF nextval('retry_attempt_seq') <= 1 THEN
      PERFORM pg_sleep(1);
    END IF;
  END;
  $$;
  CREATE EVENT TRIGGER ddl_stmt_timeout_trig ON ddl_command_start
    WHEN TAG IN ('ALTER TABLE') EXECUTE FUNCTION ddl_sleep_once();
  SET statement_timeout = '15ms';
  SET enable_ddl_autocommit_retry = off;
  SELECT setval('retry_attempt_seq', 1, false);
}
step s2_ddl_alter2  { ALTER TABLE retry_tbl ADD COLUMN extra2 int DEFAULT 99; }
step s2_ddl_stmttimeout_enable
{
  SET enable_ddl_autocommit_retry = on;
  SELECT setval('retry_attempt_seq', 1, false);
}
step s2_ddl_cleanup
{
  SET statement_timeout = 0;
  SELECT last_value AS ddl_stmttimeout_attempts FROM retry_attempt_seq;
  SELECT id, val, extra, extra2 FROM retry_tbl ORDER BY id;
  DROP EVENT TRIGGER ddl_stmt_timeout_trig;
  DROP FUNCTION ddl_sleep_once();
}

# 1. Default max_autocommit_retries = 0: serialization failure is reported
permutation s2_retries_0 s1_begin s1_upd2 s2_upd_ret s1_commit s2_check_tbl

# 2. Autocommit UPDATE ... RETURNING retries on 40001 and discards buffered rows from attempt 1
permutation s2_retries_3 s1_begin s1_upd2 s2_upd_ret s1_commit s2_check_tbl

# 3. Explicit transaction block (BEGIN ... COMMIT) does not auto-retry
permutation s2_retries_3 s1_begin s1_upd2 s2_begin s2_upd_ret s1_commit s2_rollback s2_check_tbl

# 4. Multi-statement simple query does not auto-retry
permutation s2_retries_3 s1_begin s1_upd2 s2_multistmt s1_commit s2_check_tbl

# 5. CALL procedure without internal transaction control auto-retries on 40001
permutation s2_retries_3 s1_begin s1_upd2 s2_call_simple s1_commit s2_check_tbl

# 6. CALL procedure that commits internally before failing does not auto-retry
permutation s2_retries_3 s1_begin s1_upd2 s2_call_commit s1_commit s2_check_tbl

# 7. Deterministic retry counts for 40001, 40P01, retry exhaustion, DDL 55P03, and DDL statement_timeout
permutation s2_retries_2 s2_reset_seq s2_test_40001 s2_reset_seq s2_test_40p01 s2_reset_seq s2_exhaust s2_check_seq s2_ddl_setup s2_ddl_alter s2_check_seq s2_ddl_enable s2_ddl_alter s2_ddl_stmttimeout_setup s2_ddl_alter2 s2_check_seq s2_ddl_stmttimeout_enable s2_ddl_alter2 s2_ddl_cleanup
