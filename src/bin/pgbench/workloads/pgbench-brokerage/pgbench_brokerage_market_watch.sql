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
-- pgbench-brokerage Market-Watch and Security-Detail Transaction Script
--

\set wl_id random(1, 100 * :scale)
\set co_min random(1, 40 * :scale)
\set co_max (:co_min + 10)
\set sec_idx random_zipfian(1, 100 * :scale, 1.2)
\set s_symb format_string('SYM%06d', :sec_idx)

CALL pgbench_brokerage_market_watch(:wl_id, :co_min, :co_max, 0);
CALL pgbench_brokerage_security_detail(:s_symb);
