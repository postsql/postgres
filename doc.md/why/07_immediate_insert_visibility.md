# Why: Rationale for Immediate Insert Visibility

## Motivation and Industry Context

PostgreSQL's strict multi-version concurrency control (MVCC) enforces that changes made inside a transaction are invisible to all other database connections until the transaction executes `COMMIT`.

While essential for traditional ACID business transactions, this boundary creates high latency and architectural complexity in high-throughput data-ingestion architectures:

1. **High-Throughput Ingestion Latency**:
   Applications ingesting telemetry, metrics, IoT sensor streams, or clickstream logs want newly arrived data to be queryable by dashboard readers and analytical workers immediately (sub-millisecond latency).
   - In standard PostgreSQL, workers must issue `COMMIT` very frequently to make rows visible.
   - Frequent commits cause high commit-lock contention, write-ahead logging (WAL) flush overhead, and connection pool exhaustion.
2. **Batching Trade-Off**:
   To achieve high insert throughput, applications batch thousands of rows into a single long-running transaction. However, while the batch transaction is open (often for seconds or minutes), **none of those rows are visible** to any reader. This forces a painful trade-off: high throughput vs. low read latency.
3. **Database-Backed Queues and Event Streaming**:
   When using PostgreSQL as a low-latency job queue or event pipeline, producers writing jobs into a queue table want consumers in other worker processes to pick up jobs instantly without waiting for producer batch commits.

## How This Feature Solves the Problem

1. **Decoupling Visibility from Writer Commit Boundaries**:
   With `SET immediate_insert_visibility = on`:
   - Every `INSERT` statement marks the newly created tuple as globally committed and visible immediately.
   - Concurrent reader connections see the inserted row with **zero millisecond latency**, even if the writer process keeps its database transaction open for further batch processing.

2. **Configurable Durability vs. Throughput**:
   With `immediate_insert_wal_flush`:
   - Applications that require strict crash durability can enable `immediate_insert_wal_flush = on`, forcing a synchronous WAL flush per statement.
   - Applications seeking maximum throughput (e.g. 100,000+ inserts/sec) can keep `immediate_insert_wal_flush = off`, allowing the background WAL writer to flush asynchronously while readers query the data in shared memory immediately.

3. **Safe Append-Only Semantics**:
   The feature is targeted specifically at `INSERT`. Standard MVCC safety is strictly preserved for `UPDATE` and `DELETE`, preventing write skew or lost updates while providing unprecedented real-time streaming performance.
