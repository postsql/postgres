# Why: Rationale for Logical Decoding with TIDs and REPLICA IDENTITY ROWID

## Motivation and Industry Context

Native logical replication in PostgreSQL requires that any table subject to `UPDATE` or `DELETE` operations have a **Primary Key** or a unique, non-null index designated as its `REPLICA IDENTITY`.

If a table lacks a primary key:
1. **Replication Failures on Update/Delete**:
   PostgreSQL forbids `UPDATE` and `DELETE` on the publisher unless the table is set to `REPLICA IDENTITY FULL`.
2. **Catastrophic Performance with `REPLICA IDENTITY FULL`**:
   Under `REPLICA IDENTITY FULL`, the publisher writes the entire old row into the WAL stream on every update or delete. On the subscriber, because there is no primary key index, PostgreSQL must perform a **full table sequential scan** for every single `UPDATE` or `DELETE` received from the CDC stream. If the subscriber table has 10 million rows, applying 1,000 updates results in 1,000 sequential scans of 10 million rows, completely stalling the replication apply worker and causing replication lag to explode.
3. **Database Migration Impasse**:
   Legacy databases often contain dozens or hundreds of tables designed without explicit primary keys. Migrating these databases to PostgreSQL with minimal downtime using native logical replication was previously nearly impossible without altering the production application schema.

## How This Feature Solves the Problem

1. **Native Replication without Primary Keys (`REPLICA IDENTITY ROWID`)**:
   By using the physical tuple identifier (`ctid`) as a virtual column (`.rowid` of type `tid`):
   - The publisher tracks rows by their physical tuple ID.
   - `UPDATE` and `DELETE` log only the small 6-byte TID as the key tuple, avoiding the massive WAL bloat of `REPLICA IDENTITY FULL`.

2. **Index-Only Rowid on Subscriber (Zero Disk Bloat)**:
   The subscriber creates an index on `.rowid`:
   - During CDC apply, updates and deletes perform an instant $O(\log N)$ index lookup on `.rowid`.
   - Sequential scans are completely eliminated.
   - Thanks to index-only rowid storage (`RelationIsIndexOnlyRowid`), the `.rowid` column is omitted from physical heap storage. It consumes **0 bytes of extra disk space** in the heap table.

3. **Instant Post-Migration Cleanup**:
   Once replication finishes and the cutover occurs, dropping the replication primary key is as simple as:
   ```sql
   ALTER TABLE tbl DROP COLUMN .rowid;
   ```
   Because no physical bytes were written into the heap tuples, this metadata-only change completes instantly without requiring a table rewrite, `VACUUM FULL`, or `pg_repack`.

4. **Deep Observability with `XLOG_HEAP2_PRUNE`**:
   Exposing prune records allows cache-invalidation layers and external read caches (e.g. Redis/Memcached) to be notified immediately when rows are pruned or line pointers redirected by vacuum.
