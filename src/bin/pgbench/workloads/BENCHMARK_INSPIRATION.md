# Benchmark Workload Inspiration, Domain Mapping, and IP Compliance

<!-- freshness: { owner: 'hannuk' reviewed: '2026-10-26' } -->

## 1. Overview and Principles of "Inspiration" vs. "Certification"

The benchmark workload suites included with `pgbench` are independent engineering benchmarks designed for database engine research, kernel regression detection, performance evaluation, and continuous integration testing.

A foundational distinction exists in intellectual property law and governance policies between:
1. **Official Certified Benchmarks**: Proprietary benchmarks developed, governed, audited, and published by formal industry consortia (such as the Transaction Processing Performance Council (TPC) and the Graph Data Council (LDBC)). Official benchmark results require strict adherence to full specifications, formal pricing and full-disclosure reports (FDR), independent auditor verification, and council certification.
2. **Independent "Inspired-by" Research Suites**: Clean-room open-source engineering implementations that adopt well-known conceptual transaction processing, analytical decision support, and graph traversal patterns to simulate realistic database workloads. Under 17 U.S.C. § 102(b), copyright does not extend to ideas, procedures, systems, methods of operation, concepts, principles, or mathematical formulas. All schemas, generators, and queries in `pgbench` are 100% original, clean-room implementations written in native SQL and C without copying third-party code.

To ensure strict compliance with trademark and fair-use governance policies (including TPC Policies v7.x Section 8 and the LDBC Byelaws & Trademark/Fair Use Policy):
- **No Trademarked Suite Names**: Suitenames and directory paths avoid using registered trademarks (such as `TPC-C`, `TPC-E`, `TPC-H`, `TPC-DS`, `LDBC`, or `VecBench`). Instead, suites are named using clear, domain-descriptive business terminology (`pgbench-banking`, `pgbench-order`, `pgbench-brokerage`, `pgbench-analytics`, `pgbench-retail`, `pgbench-htap`, `pgbench-graph`, `pgbench-vec`).
- **No Trademarked Metric Reporting**: Restrictive, audited metric titles such as `tpmC` and `QphH` are never emitted. Instead, `pgbench` reports standard unencumbered physical rate units:
  - `nopm`: New Orders Per Minute (standard industry throughput metric for wholesale order entry, aligned with tools such as HammerDB and Sysbench).
  - `qph@SF`: Queries Per Hour at Scale Factor (standard rate unit for analytical decision support queries).
- **Mathematical Terminology for Graph Primitives**: Generator functions avoid consortium acronyms (`ldbc_*`) and are named after the underlying mathematical algorithms:
  - `community_correlated_edge`: Stochastic block model with attribute homophily and power-law degree distributions.
  - `burst_timestamp`: Multi-modal Gaussian temporal clustering modeling traffic spikes and bursts.

---

## 2. Workload Correspondence and Alignment Table

The following table provides the exact alignment between each `pgbench` workload suite, the domain concepts and industry specifications that inspired it, corresponding external open-source tools (such as HammerDB and Sysbench), and emitted primary throughput metrics:

| Workload Suite | Inspired By | Conceptual Domain / Workload Pattern | External Tools Alignment | Emitted Primary Metric |
| :--- | :--- | :--- | :--- | :--- |
| **`pgbench-banking`** | Wisconsin / TPC-B / Standard OLTP | Account debit/credit transactions, teller balancing, history logging via stored procedures | Sysbench OLTP, pgbench built-in | `tps` (Transactions Per Second) |
| **`pgbench-order`** | TPC-C | Multi-warehouse wholesale order-entry, delivery processing, stock-level checks, customer payments | HammerDB TPROC-C, Sysbench OLTP complex | `nopm` (New Orders Per Minute) |
| **`pgbench-brokerage`** | TPC-E | Financial securities trading, customer portfolio analysis, trade orders, market watch feeds | HammerDB TPROC-E | `tps` (Transactions Per Second) |
| **`pgbench-analytics`** | TPC-H | Decision support analytical reporting, ad-hoc business queries (Q1–Q19), concurrent refresh streams | HammerDB TPROC-H | `qph@SF` (Queries Per Hour at SF) |
| **`pgbench-retail`** | TPC-DS | Star-schema multi-channel retail analytics, inventory, store sales, cross-channel promotion queries | HammerDB TPROC-DS | `qph@SF` (Queries Per Hour at SF) |
| **`pgbench-htap`** | CH-benCHmark | Mixed hybrid workload combining 9-table order-entry OLTP with simultaneous 22-query analytical DSS | CH-benCHmark research suite | `nopm` (OLTP) + `qph` (OLAP) |
| **`pgbench-graph`** | LDBC SNB, LSQB, FinBench, Graphalytics | Social network traversals, subgraph pattern matching, recursive CTEs, ISO SQL/PGQ `GRAPH_TABLE` | LDBC reference suites | `tps` (Transactions Per Second) |
| **`pgbench-vec`** | VecBench (ACM SIGMOD) | Controllable high-dimensional filtered vector search, hybrid HNSW/IVFFlat index aging, Recall@K | VecBench academic harness | QPS + Recall@K |

---

## 3. Metric and Unit Definitions

1. **`nopm` (New Orders Per Minute)**:
   Calculated as `(new_order_transactions * 60.0) / duration_seconds`. This represents the completed throughput of the primary business transaction in order-entry benchmarks. Unlike `tpmC`, which is a legally restricted certification mark requiring full-disclosure pricing and audited client emulation, `nopm` is an objective, unencumbered measurement unit widely recognized across database benchmarking ecosystems.

2. **`qph@SF` (Queries Per Hour at Scale Factor)**:
   Calculated as `(completed_analytical_queries * 3600.0 * scale) / duration_seconds`. This measures the aggregate throughput of analytical query streams normalized by scale factor.

3. **`community_correlated_edge(src_id, num_vertices, num_communities, homophily_prob [, seed])`**:
   Implements a stochastic block model where vertex `src_id` connects with probability `homophily_prob` to another vertex within the same community, or with probability `(1.0 - homophily_prob)` across communities, scaled by a power-law degree exponent.

4. **`burst_timestamp(base_ts_us, span_us, num_spikes, spike_prob [, seed])`**:
   Generates a non-uniform timestamp in microseconds modeling clustered traffic bursts using multi-modal Gaussian distribution spikes superimposed over uniform background traffic.

---

## 4. Formal Legal Disclaimers

### 4.1 Relational OLTP and Decision Support Workloads
The `pgbench-banking`, `pgbench-order`, `pgbench-brokerage`, `pgbench-analytics`, `pgbench-retail`, and `pgbench-htap` workloads are independent engineering benchmarks. While inspired by industry transactional and decision support concepts, these suites are NOT official TPC Benchmarks, have NOT been audited or certified by the Transaction Processing Performance Council (TPC), and do NOT produce official TPC Benchmark Results (such as `tpmC` or `QphH`). TPC, TPC-C, TPC-E, TPC-H, TPC-DS, tpmC, and QphH are registered trademarks of the Transaction Processing Performance Council.

### 4.2 Graph Benchmark Workloads
The `pgbench-graph`, `pgbench-graph-sql-ud`, `pgbench-graph-pgq`, and `pgbench-graph-plpgq` workloads are independent engineering benchmarks. While inspired by graph traversal patterns published by the Graph Data Council (LDBC), these suites are NOT LDBC Benchmarks, are NOT implementations of LDBC Benchmarks, and these results are NOT LDBC Benchmark Results. LDBC, LDBC Benchmark, and LDBC Benchmark Results are registered trademarks of the Graph Data Council. Distributed under CC-BY 4.0 attribution terms.

### 4.3 Filtered Vector Search Workloads
The `pgbench-vec` workload is an independent engineering benchmark inspired by the filtered vector search methodologies described in *"VecBench: A Controllable Benchmark for Filtered Vector Search"* (Zhang et al., ACM SIGMOD). It is not the official VecBench software suite and does not publish official VecBench leaderboard ratings.
