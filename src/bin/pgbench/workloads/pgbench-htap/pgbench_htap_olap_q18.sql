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
-- CH-benCHmark Analytical Query 18: Large Volume Customer Query
--

SELECT
    c_last,
    c_id,
    o_id,
    o_entry_d,
    o_ol_cnt,
    sum(ol_amount) AS amount_sum
FROM
    customer,
    orders,
    order_line
WHERE
    c_id = o_c_id
    AND c_w_id = o_w_id
    AND c_d_id = o_d_id
    AND ol_w_id = o_w_id
    AND ol_d_id = o_d_id
    AND ol_o_id = o_id
GROUP BY
    o_id,
    o_w_id,
    o_d_id,
    c_id,
    c_last,
    o_entry_d,
    o_ol_cnt
HAVING
    sum(ol_amount) > 50.00
ORDER BY
    amount_sum DESC,
    o_entry_d
LIMIT 10;
