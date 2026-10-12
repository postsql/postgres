# What: Logical Decoding with TIDs and REPLICA IDENTITY ROWID

## Overview

The `logical-decoding-with-tids` patch enables full tuple identifier (TID) tracking across logical decoding and native logical replication, solving major historical limitations for tables without primary keys:

![Logical Decoding Rowid](../img/logical_decoding_rowid.svg)

1. **Tuple Identifier Exposure in Logical Decoding**:
   - Logical decoding output plugins can receive the physical tuple identifier (`ctid` of type `ItemPointerData` / `tid`) for every change.
   - For `test_decoding`, this is exposed via the plugin option `include-tids` (e.g., `START_REPLICATION SLOT slot_name LOGICAL ... (include-tids '1')`).
   - Changes are output with physical coordinates: `table public.t: INSERT (tid[0,1]): id[integer]:1`.

2. **`REPLICA IDENTITY ROWID` for Native Logical Replication**:
   - Adds `ALTER TABLE tbl REPLICA IDENTITY ROWID;` (`'r'` in `pg_class.relreplident`).
   - Allows native logical replication on tables **without a primary key or unique index**.
   - The publisher exposes the physical tuple identifier as a virtual column named `.rowid` of type `tid`.
   - On `INSERT`, the newly created TID is sent as `.rowid`.
   - On `UPDATE`, the old TID is sent in the key tuple and the new TID in the new tuple.
   - On `DELETE`, the old TID is sent in the key tuple, enabling the subscriber to identify and delete the exact row without performing an expensive full table sequential scan.

3. **Index-Only Primary Key (`.rowid`) on Subscriber**:
   - On subscriber tables, the virtual column `.rowid` can be indexed (`CREATE INDEX ON tbl(.rowid)`).
   - When `logical_replication_index_only_rowid = true` (or table reloption `WITH (index_only_rowid = true)`):
     - The `.rowid` column is **omitted from physical heap tuple storage** (`desc->natts - 1`).
     - Zero bytes of heap bloat are consumed on disk for `.rowid`.
     - Index entries are still maintained, allowing $O(\log N)$ B-Tree lookups for CDC updates and deletes.
     - Post-migration, the `.rowid` column and index can be dropped instantly with zero table rewrite and no `VACUUM FULL`.

4. **Logical Decoding of `XLOG_HEAP2_PRUNE` Records**:
   - Introduces WAL logging and decoding of vacuum and opportunistic pruning records when `logical_decoding_prune_records = true`.
   - Allows replication clients to observe line pointer redirection, deletion, and freezing in real time via the `prune_cb` output plugin callback (or `include-prunes` option in `test_decoding`).
