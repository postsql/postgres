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
-- TPC-DS Inventory Fluctuation Query (Q21 / Q22 style)
--

\set day_off random(35, 300)
\set target_date int_to_date('2000-01-01', :day_off, 'day')
\set date_before date_add(:target_date, -30, 'day')
\set date_after date_add(:target_date, 30, 'day')

SELECT
    w_warehouse_name,
    i_item_id,
    i_category,
    sum(CASE WHEN d_date < :target_date::date
             THEN inv_quantity_on_hand ELSE 0 END) AS inv_before,
    sum(CASE WHEN d_date >= :target_date::date
             THEN inv_quantity_on_hand ELSE 0 END) AS inv_after
FROM
    inventory
    JOIN warehouse ON inv_warehouse_sk = w_warehouse_sk
    JOIN item ON inv_item_sk = i_item_sk
    JOIN date_dim ON inv_date_sk = d_date_sk
WHERE
    d_date BETWEEN :date_before::date AND :date_after::date
GROUP BY
    w_warehouse_name,
    i_item_id,
    i_category
ORDER BY
    w_warehouse_name,
    i_item_id
LIMIT 25;
