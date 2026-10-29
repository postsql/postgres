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
-- pgbench-retail (TPC-DS-inspired) Data Maintenance Script
--
-- Executes retail maintenance via stored procedure to insert a batch of new store_sales rows
-- and purge expired maintenance records.
--

\set batch_size 4
\set base_ticket (9000000 + :client_id * 10000 + random(1, 5000))

CALL pgbench_retail_maintenance(:base_ticket, :batch_size, :scale);
