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
-- pgbench-c (TPC-C-inspired) Stock-Level Transaction Script
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)
\set threshold random(10, 20)

CALL pgbench_c_stock_level(:w_id, :d_id, :threshold, 0);
\think_time random_exponential(1, 5, 2.0) ms
