/*
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * DISCLAIMER: This workload is an independent engineering benchmark designed
 * for database performance research, development, and regression testing.
 * While inspired by industry transactional and decision support concepts,
 * this suite is NOT an official TPC Benchmark, has NOT been audited or
 * certified by the Transaction Processing Performance Council (TPC), and
 * does NOT produce official TPC Benchmark Results (such as tpmC or QphH).
 * TPC, TPC-C, TPC-E, TPC-H, TPC-DS, tpmC, and QphH are registered trademarks
 * of the Transaction Processing Performance Council.
 */

--
-- pgbench-b (modernized, database-function based Simple-Update transaction)
--
-- Executes the standard 3-statement Simple-Update transaction inside a single
-- stored procedure call, eliminating client-server roundtrips and
-- running cleanly under PostgreSQL autocommit mode.
--

\set aid random(1, 100000 * :scale)
\set bid random(1, :scale)
\set tid random(1, 10 * :scale)
\set delta random(-5000, 5000)

CALL pgbench_b_simple_update(:aid, :bid, :tid, :delta);
