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
-- TPC-C Post-Load Secondary Indexes, Foreign Keys, and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_customer_name
    ON customer (c_w_id, c_d_id, c_last, c_first);

CREATE INDEX IF NOT EXISTS idx_orders_customer
    ON orders (o_w_id, o_d_id, o_c_id, o_id DESC);

ANALYZE warehouse, district, customer, history, new_order, orders, order_line, item, stock;
