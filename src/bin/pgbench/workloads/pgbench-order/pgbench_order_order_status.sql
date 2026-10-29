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
-- pgbench-order (TPC-C-inspired) Order-Status Transaction Script
--
-- Features:
--   - 60% customer lookup by c_last(nurand(255, 0, 999)) / 40% by c_id
--

\keying_time 1 ms
\set w_id random(1, :scale)
\set d_id random(1, 10)

\if random(1, 100) <= 60
  \set c_last c_last(nurand(255, 0, 999))
  CALL pgbench_order_order_status(:w_id, :d_id, NULL, :c_last);
\else
  \set c_id nurand(1023, 1, 30)
  CALL pgbench_order_order_status(:w_id, :d_id, :c_id, NULL);
\endif
\think_time random_exponential(1, 5, 2.0) ms
