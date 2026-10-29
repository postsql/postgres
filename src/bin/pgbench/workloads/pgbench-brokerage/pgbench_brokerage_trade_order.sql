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
-- pgbench-brokerage (TPC-E-inspired) Trade-Order Transaction Script
--
-- Features:
--   - Skewed Zipfian security selection via random_zipfian() and permute()
--   - 1% intentional rollback caught via \try ... \catch 'P0002'
--

\set num_accounts (200 * :scale)
\set num_securities (100 * :scale)
\set ca_id random(1, :num_accounts)
\set sec_rank random_zipfian(1, :num_securities, 1.2)
\set sec_idx 1 + permute(:sec_rank, :num_securities)
\set s_symb format_string('SYM%06d', :sec_idx)
\set tt_choice random(1, 100)
\set tt_id CASE WHEN :tt_choice <= 30 THEN 'TMB' WHEN :tt_choice <= 60 THEN 'TMS' WHEN :tt_choice <= 80 THEN 'TLB' ELSE 'TLS' END
\set qty random(1, 8) * 100
\set is_lifo (random(1, 100) <= 35)
\set roll_it_back (random(1, 100) = 1)

\try
CALL pgbench_brokerage_trade_order(:ca_id, :s_symb, :tt_id, :qty, :is_lifo, :roll_it_back, NULL);
\catch 'P0002'
\endtry
