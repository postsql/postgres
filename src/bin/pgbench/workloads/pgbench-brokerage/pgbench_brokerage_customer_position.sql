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
-- pgbench-brokerage (TPC-E-inspired) Customer-Position Transaction Script
--
-- Features:
--   - Single in-engine stored procedure call performing customer account lookup (Frame 1)
--   - Captures execution metadata via \gset_meta
--

\set c_id random(1, 100 * :scale)
\set get_hist (random(1, 100) <= 50)

CALL pgbench_brokerage_customer_position(:c_id, :get_hist) \gset_meta cp_
