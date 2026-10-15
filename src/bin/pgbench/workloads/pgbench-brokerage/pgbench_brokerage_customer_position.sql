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
-- pgbench-e (TPC-E-inspired) Customer-Position Transaction Script
--
-- Features:
--   - Captures customer's account ID array into :ca_ids via \gset
--   - Uses 0-based array subscripting :ca_ids[0]
--   - Captures execution metadata via \gset_meta
--

\set c_id random(1, 100 * :scale)
\set get_hist (random(1, 100) <= 50)

SELECT c_id AS cust_id,
       ARRAY(SELECT ca_id FROM customer_account WHERE ca_c_id = :c_id ORDER BY ca_id LIMIT 10) AS ca_ids
  FROM customer
 WHERE c_id = :c_id \gset

\set target_ca :ca_ids[0]
CALL pgbench_e_customer_position(:cust_id, :target_ca, :get_hist) \gset_meta cp_
