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
-- pgbench-b Post-Load Constraints, Indexes, and Vacuum Analyze
--

ALTER TABLE pgbench_branches ADD PRIMARY KEY (bid);
ALTER TABLE pgbench_tellers ADD PRIMARY KEY (tid);
ALTER TABLE pgbench_accounts ADD PRIMARY KEY (aid);

ALTER TABLE pgbench_tellers ADD CONSTRAINT pgbench_tellers_bid_fkey
    FOREIGN KEY (bid) REFERENCES pgbench_branches(bid);
ALTER TABLE pgbench_accounts ADD CONSTRAINT pgbench_accounts_bid_fkey
    FOREIGN KEY (bid) REFERENCES pgbench_branches(bid);

VACUUM ANALYZE pgbench_branches;
VACUUM ANALYZE pgbench_tellers;
VACUUM ANALYZE pgbench_accounts;
VACUUM ANALYZE pgbench_history;
