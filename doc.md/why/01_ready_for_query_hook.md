# Why: Rationale for ReadyForQuery Extension and Hook

## Motivation and Industry Context

In modern distributed PostgreSQL architectures, connection poolers (such as PgBouncer, Odyssey, and Envoy) and smart database proxies sit between thousands of application workers and a cluster of database instances.

To achieve high concurrency, poolers operate in **transaction pooling mode**, where a backend database connection is loaned to a client only for the duration of a single transaction and returned to the pool immediately upon `COMMIT` or `ROLLBACK`.

However, transaction pooling suffers from severe state-leakage pitfalls:
1. **Hidden Session State**:
   If a client creates a temporary table, executes a prepared statement, or opens a cursor with `WITH HOLD`, that state resides in the backend session. If the pooler returns that connection to another client, subsequent queries may fail, see unintended data, or leak resources.
2. **Expensive State Probing**:
   Without wire protocol signals, poolers must either:
   - Run `DISCARD ALL` or `DISCARD TEMP` after every transaction, which destroys prepared statement caches and adds costly network and query overhead.
   - Forbid prepared statements and temporary tables entirely in transaction pooling mode.
3. **Read-After-Write Consistency Lag in Read Replicas**:
   Clients routing writes to the primary and reads to standby replicas need to ensure that the replica has replayed at least up to the primary's commit LSN (causal consistency). Traditionally, retrieving the commit LSN requires executing an extra query (`SELECT pg_current_wal_insert_lsn()`) after every commit, doubling transaction latency.

## How This Feature Solves the Problem

1. **Zero-Overhead Session State Signaling**:
   By adding `'T'` (temp tables), `'H'` (hold cursors), and `'P'` (prepared statements) directly to the terminal `ReadyForQuery` packet of each command:
   - The connection pooler reads these flags in $O(1)$ from the final wire packet with **zero extra round-trips**.
   - If no session state exists (`T:0, H:0, P:0`), the connection is safely returned to the pool immediately.
   - If session state was introduced, the pooler can either keep the connection pinned to that client or issue a targeted cleanup command.

2. **Causal Replica Routing via Commit LSN**:
   The `'l'` flag directly delivers the 8-byte commit LSN in the `ReadyForQuery` packet. Proxies can immediately inspect this value and route subsequent read queries to a read replica that has caught up past that LSN, guaranteeing monotonic read consistency without any client-side coordination queries.

3. **Extensibility for Enterprise Mesh and Observability**:
   The C hook (`ReadyForQueryHook`) lets custom extensions append arbitrary telemetry—such as request tracing IDs, tenant quotas, query execution costs, or cache invalidation tags—directly into the connection boundary.
