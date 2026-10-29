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
-- pgbench-brokerage Trade-Result and Market-Feed Transaction Script
--

\set num_trades (300 * :scale)
\set num_securities (100 * :scale)
\set t_id random(1, :num_trades)
\set trade_price random_gaussian(2000, 8000, 2.5) / 100.0
\set sec_idx random_zipfian(1, :num_securities, 1.2)
\set s_symb format_string('SYM%06d', :sec_idx)
\set feed_qty random(1, 10) * 100

CALL pgbench_brokerage_trade_result(:t_id, :trade_price);
CALL pgbench_brokerage_market_feed(:s_symb, :trade_price, :feed_qty);
