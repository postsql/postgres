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
-- pgbench-analytics (TPC-H-inspired) Query 3: Shipping Priority Query
--

\set seg_idx random(0, 4)
\set segment CASE WHEN :seg_idx = 0 THEN 'AUTOMOBILE' WHEN :seg_idx = 1 THEN 'BUILDING' WHEN :seg_idx = 2 THEN 'FURNITURE' WHEN :seg_idx = 3 THEN 'MACHINERY' ELSE 'HOUSEHOLD' END
\set day_off random(0, 30)
\set cutoff_date int_to_date('1995-03-01', :day_off, 'day')

SELECT
    l_orderkey,
    sum(l_extendedprice * (1 - l_discount)) AS revenue,
    o_orderdate,
    o_shippriority
FROM
    customer,
    orders,
    lineitem
WHERE
    c_mktsegment = :segment
    AND c_custkey = o_custkey
    AND l_orderkey = o_orderkey
    AND o_orderdate < :cutoff_date::date
    AND l_shipdate > :cutoff_date::date
GROUP BY
    l_orderkey,
    o_orderdate,
    o_shippriority
ORDER BY
    revenue DESC,
    o_orderdate
LIMIT 10;
