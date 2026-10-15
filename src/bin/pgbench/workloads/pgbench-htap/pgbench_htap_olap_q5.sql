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
-- CH-benCHmark Analytical Query 5: Local Supplier Volume Query
--
-- Joins operational tables (customer, orders, order_line, stock) with
-- analytical dimension tables (htap_supplier, htap_nation, htap_region).
--

\set reg_idx random(0, 4)
\set r_name CASE WHEN :reg_idx = 0 THEN 'AFRICA' WHEN :reg_idx = 1 THEN 'AMERICA' WHEN :reg_idx = 2 THEN 'ASIA' WHEN :reg_idx = 3 THEN 'EUROPE' ELSE 'MIDDLE EAST' END

SELECT
    n_name,
    round(sum(ol_amount), 2) AS revenue
FROM
    customer,
    orders,
    order_line,
    stock,
    htap_supplier,
    htap_nation,
    htap_region
WHERE
    c_w_id = o_w_id
    AND c_d_id = o_d_id
    AND c_id = o_c_id
    AND o_w_id = ol_w_id
    AND o_d_id = ol_d_id
    AND o_id = ol_o_id
    AND ol_w_id = s_w_id
    AND ol_i_id = s_i_id
    AND ((s_w_id * s_i_id) % 100) + 1 = su_suppkey
    AND ascii(substr(c_state, 1, 1)) % 25 = su_nationkey
    AND su_nationkey = n_nationkey
    AND n_regionkey = r_regionkey
    AND r_name = :r_name
GROUP BY
    n_name
ORDER BY
    revenue DESC;
