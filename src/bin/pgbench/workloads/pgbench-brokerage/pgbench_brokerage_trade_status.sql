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
-- TPC-E Trade-Status, Trade-Lookup, and Trade-Update Transaction Script
--

\set ca_id random(1, 200 * :scale)
\set op random(1, 100)

\if :op <= 60
  CALL pgbench_e_trade_status(:ca_id);
\elif :op <= 90
  CALL tpce_trade_lookup(:ca_id, 10);
\else
  CALL tpce_trade_update(:ca_id, 5);
\endif
