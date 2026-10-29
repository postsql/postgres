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
-- pgbench-analytics (TPC-H-inspired) Refresh Functions (RF1 New Sales Insert & RF2 Old Sales Delete)
--
-- Executes RF1 and RF2 via stored procedures to insert a batch of new orders + lineitems (RF1)
-- and purge the refreshed batch (RF2).
--

\set batch_size 5
\set base_okey (9000000 + :client_id * 10000 + random(1, 5000))

CALL pgbench_analytics_rf1(:base_okey, :batch_size, :scale);
CALL pgbench_analytics_rf2(:base_okey, :batch_size);
