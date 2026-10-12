--
-- AUTONOMOUS TRANSACTIONS (BEGIN AUTONOMOUS ... COMMIT AUTONOMOUS)
--

CREATE TABLE auto_test (
    id int PRIMARY KEY,
    val text
);

CREATE TABLE auto_audit (
    seq serial PRIMARY KEY,
    msg text
);

-- 1. Top-level BEGIN AUTONOMOUS / COMMIT AUTONOMOUS and ROLLBACK
BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (1, 'top-level autonomous commit');
COMMIT AUTONOMOUS;

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (2, 'top-level autonomous rollback');
ROLLBACK;

SELECT * FROM auto_test ORDER BY id;

-- 2. Nested BEGIN AUTONOMOUS survives outer transaction ROLLBACK
BEGIN;
INSERT INTO auto_test VALUES (10, 'outer uncommitted');

BEGIN AUTONOMOUS;
INSERT INTO auto_audit (msg) VALUES ('logged from autonomous xact 1');
INSERT INTO auto_test VALUES (11, 'inner autonomous committed');
COMMIT AUTONOMOUS;

-- Outer transaction in READ COMMITTED sees committed autonomous changes
SELECT * FROM auto_test ORDER BY id;

INSERT INTO auto_test VALUES (12, 'outer uncommitted 2');
ROLLBACK;

-- Only autonomous changes (id=1, 11) and audit log remain
SELECT * FROM auto_test ORDER BY id;
SELECT msg FROM auto_audit ORDER BY seq;

-- 3. Error and ROLLBACK inside nested BEGIN AUTONOMOUS resumes outer transaction
BEGIN;
INSERT INTO auto_test VALUES (20, 'outer before failed autonomous');

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (21, 'autonomous before error');
-- Duplicate key error aborts only the autonomous transaction
INSERT INTO auto_test VALUES (1, 'duplicate key');
COMMIT AUTONOMOUS;

-- We are back in the outer transaction and it is still valid
INSERT INTO auto_test VALUES (22, 'outer after failed autonomous');
COMMIT;

SELECT * FROM auto_test WHERE id >= 20 ORDER BY id;

-- Explicit ROLLBACK of nested autonomous transaction
BEGIN;
INSERT INTO auto_test VALUES (30, 'outer before explicit autonomous rollback');

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (31, 'autonomous rolled back explicitly');
ROLLBACK;

INSERT INTO auto_test VALUES (32, 'outer after explicit autonomous rollback');
COMMIT;

SELECT * FROM auto_test WHERE id >= 30 ORDER BY id;

-- 4. Multiple sequential and doubly-nested autonomous transactions
BEGIN;
INSERT INTO auto_test VALUES (40, 'outer rolled back');

BEGIN AUTONOMOUS;
INSERT INTO auto_audit (msg) VALUES ('outer autonomous rolled back');

    BEGIN AUTONOMOUS;
    INSERT INTO auto_audit (msg) VALUES ('innermost autonomous committed');
    COMMIT AUTONOMOUS;

ROLLBACK;

BEGIN AUTONOMOUS;
INSERT INTO auto_audit (msg) VALUES ('second autonomous committed');
COMMIT AUTONOMOUS;

ROLLBACK;

SELECT msg FROM auto_audit ORDER BY seq;

-- 5. Savepoints inside BEGIN AUTONOMOUS and BEGIN AUTONOMOUS inside a savepoint
BEGIN;
SAVEPOINT sp_outer;
INSERT INTO auto_test VALUES (50, 'in sp_outer');

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (51, 'in autonomous before sp_inner');
SAVEPOINT sp_inner;
INSERT INTO auto_test VALUES (52, 'in sp_inner rolled back');
ROLLBACK TO SAVEPOINT sp_inner;
INSERT INTO auto_test VALUES (53, 'in autonomous after sp_inner');
RELEASE SAVEPOINT sp_inner;
COMMIT AUTONOMOUS;

-- Cannot rollback or release outer savepoint across autonomous boundary
BEGIN AUTONOMOUS;
ROLLBACK TO SAVEPOINT sp_outer;
ROLLBACK;

-- Back in sp_outer; rolling back sp_outer undoes 50, but 51 and 53 survive
ROLLBACK TO SAVEPOINT sp_outer;
COMMIT;

SELECT * FROM auto_test WHERE id >= 50 ORDER BY id;

-- 6. Snapshot isolation: outer REPEATABLE READ vs inner BEGIN AUTONOMOUS
BEGIN ISOLATION LEVEL REPEATABLE READ;
SELECT count(*) AS before_auto FROM auto_test WHERE id >= 60;

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (60, 'committed in autonomous while parent is RR');
COMMIT AUTONOMOUS;

-- Outer REPEATABLE READ snapshot must NOT see id=60
SELECT count(*) AS in_rr_parent_after_auto FROM auto_test WHERE id >= 60;
COMMIT;

-- Now visible after outer transaction ends
SELECT * FROM auto_test WHERE id >= 60 ORDER BY id;

-- 7. Outer READ ONLY transaction with inner READ WRITE autonomous transaction
BEGIN READ ONLY;
INSERT INTO auto_test VALUES (70, 'should fail in read only parent');
ROLLBACK;

BEGIN READ ONLY;
BEGIN AUTONOMOUS READ WRITE;
INSERT INTO auto_audit (msg) VALUES ('written from autonomous inside read-only parent');
COMMIT AUTONOMOUS;
-- Parent is still read-only
INSERT INTO auto_test VALUES (70, 'should still fail in read only parent');
ROLLBACK;

SELECT msg FROM auto_audit ORDER BY seq;

-- 8. Self-deadlock detection when autonomous xact conflicts with suspended parent lock
BEGIN;
UPDATE auto_test SET val = 'locked by parent' WHERE id = 1;

BEGIN AUTONOMOUS;
-- Must immediately detect self-deadlock with suspended parent transaction
UPDATE auto_test SET val = 'conflict from autonomous' WHERE id = 1;
ROLLBACK;

-- Parent transaction is unaffected and can commit its update
COMMIT;
SELECT * FROM auto_test WHERE id = 1;

-- 9. Plain COMMIT inside nested BEGIN AUTONOMOUS and COMMIT AUTONOMOUS outside autonomous
BEGIN;
BEGIN AUTONOMOUS;
-- Plain COMMIT is rejected inside a nested autonomous transaction
COMMIT;
COMMIT AUTONOMOUS;
-- COMMIT AUTONOMOUS is rejected when not in an autonomous transaction
COMMIT AUTONOMOUS;
ROLLBACK;

-- 10. GUC SET / SET LOCAL isolation across BEGIN AUTONOMOUS
SET work_mem = '4MB';
BEGIN;
SET LOCAL work_mem = '8MB';
SHOW work_mem;

BEGIN AUTONOMOUS;
SET LOCAL work_mem = '16MB';
SHOW work_mem;
COMMIT AUTONOMOUS;

-- Outer transaction's SET LOCAL is still 8MB
SHOW work_mem;
COMMIT;
SHOW work_mem;
RESET work_mem;

-- 11. Deferred constraint triggers inside BEGIN AUTONOMOUS
CREATE TABLE auto_trig_log (msg text);

CREATE FUNCTION auto_defer_trig_fn() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN
    INSERT INTO auto_trig_log VALUES ('fired for id=' || NEW.id);
    RETURN NEW;
END;
$$;

CREATE CONSTRAINT TRIGGER auto_defer_trig
    AFTER INSERT ON auto_test
    DEFERRABLE INITIALLY DEFERRED
    FOR EACH ROW EXECUTE FUNCTION auto_defer_trig_fn();

BEGIN;
INSERT INTO auto_test VALUES (80, 'outer deferred');

BEGIN AUTONOMOUS;
INSERT INTO auto_test VALUES (81, 'autonomous deferred');
-- Trigger for 81 fires at COMMIT AUTONOMOUS; trigger for 80 has not fired yet
COMMIT AUTONOMOUS;

SELECT * FROM auto_trig_log ORDER BY msg;
-- Rollback outer transaction so 80 never fires
ROLLBACK;

SELECT * FROM auto_trig_log ORDER BY msg;

-- 12. CommandId (cmin) and snapshot isolation across BEGIN AUTONOMOUS
CREATE TABLE auto_cid_outer (id int);
CREATE TABLE auto_cid_inner (id int);

BEGIN;
INSERT INTO auto_cid_outer VALUES (1);
INSERT INTO auto_cid_outer VALUES (2);

BEGIN AUTONOMOUS;
INSERT INTO auto_cid_inner VALUES (10);
SELECT cmin AS auto_first_cmin, id FROM auto_cid_inner;
INSERT INTO auto_cid_inner VALUES (11);
COMMIT AUTONOMOUS;

INSERT INTO auto_cid_outer VALUES (3);
SELECT cmin AS outer_cmin, id FROM auto_cid_outer ORDER BY id;
ROLLBACK;

DROP TABLE auto_cid_inner;
DROP TABLE auto_cid_outer;
DROP TABLE auto_trig_log;
DROP FUNCTION auto_defer_trig_fn() CASCADE;
DROP TABLE auto_audit;
DROP TABLE auto_test;
