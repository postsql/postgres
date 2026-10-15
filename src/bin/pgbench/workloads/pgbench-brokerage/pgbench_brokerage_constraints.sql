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
-- TPC-E Post-Load Secondary Indexes and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_customer_account_cid
    ON customer_account (ca_c_id);

CREATE INDEX IF NOT EXISTS idx_trade_ca_dts
    ON trade (t_ca_id, t_dts DESC);

CREATE INDEX IF NOT EXISTS idx_trade_st_id
    ON trade (t_st_id);

CREATE INDEX IF NOT EXISTS idx_security_co_id
    ON security (s_co_id);

ANALYZE broker, company, security, last_trade, customer, customer_account,
        holding, holding_summary, watch_list, watch_item,
        trade_type, trade, trade_history, trade_request,
        cash_transaction, settlement;
