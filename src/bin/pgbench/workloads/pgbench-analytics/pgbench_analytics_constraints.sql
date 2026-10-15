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
-- TPC-H Post-Load Secondary Indexes and ANALYZE
--

CREATE INDEX IF NOT EXISTS idx_lineitem_shipdate
    ON lineitem (l_shipdate);

CREATE INDEX IF NOT EXISTS idx_lineitem_part_supp
    ON lineitem (l_partkey, l_suppkey);

CREATE INDEX IF NOT EXISTS idx_orders_custkey_orderdate
    ON orders (o_custkey, o_orderdate);

CREATE INDEX IF NOT EXISTS idx_customer_nationkey_mktsegment
    ON customer (c_nationkey, c_mktsegment);

CREATE INDEX IF NOT EXISTS idx_supplier_nationkey
    ON supplier (s_nationkey);

ANALYZE region, nation, supplier, customer, part, partsupp, orders, lineitem;
