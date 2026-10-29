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
-- pgbench-analytics (TPC-H-inspired) Query 6: Forecasting Revenue Change Query
--

\set yr_off random(0, 4)
\set start_date int_to_date('1993-01-01', :yr_off, 'year')
\set end_date date_add(:start_date, 1, 'year')
\set disc_int random(2, 9)
\set disc_lo (:disc_int - 1) / 100.0
\set disc_hi (:disc_int + 1) / 100.0
\set qty random(24, 25)

SELECT
    sum(l_extendedprice * l_discount) AS revenue
FROM
    lineitem
WHERE
    l_shipdate >= :start_date::date
    AND l_shipdate < :end_date::date
    AND l_discount BETWEEN :disc_lo AND :disc_hi
    AND l_quantity < :qty;
