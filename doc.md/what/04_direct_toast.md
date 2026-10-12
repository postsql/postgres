# What: Direct TOAST Storage Engine

## Overview

Direct TOAST is a modern physical storage engine and pointer architecture for out-of-line variable-length attribute storage (TOAST). It replaces PostgreSQL's legacy Plain TOAST indirection scheme with direct, index-free tuple addressing:

![Direct TOAST Architecture](../img/direct_toast_architecture.svg)

1. **Direct Pointer Representation (`VARTAG_DIRECT`)**:
   - In Plain TOAST (`VARTAG_ONDISK`), external pointers store a 32-bit chunk ID (`OID`). Every access requires traversing a B-Tree index on `(chunk_id, chunk_seq)` to find the physical disk blocks of the chunks.
   - In Direct TOAST, the external pointer (`struct varatt_direct`) stores the physical **ItemPointer (TID)** of the root chunk directly: `[va_rawsize (30 bits), va_extinfo (32 bits), va_chunk_tid (6 bytes ItemPointerData)]`.
   - Access to TOAST data is $O(1)$ directly by block number and page offset, bypassing B-Tree traversal entirely.

2. **Three-Tier Storage Hierarchy**:
   - **Tier 1: Single Chunk**: For values up to ~2KB, the single chunk is addressed directly by the root `va_chunk_tid`.
   - **Tier 2: Flat Array (`VARTAG_DIRECT_FLAT`)**: For values up to ~64KB–128KB, the root chunk stores a header containing an array of ItemPointers (`ItemPointerData chunks[]`) pointing directly to each subsequent data chunk.
   - **Tier 3: Hierarchical DAG (`VARTAG_DIRECT_DAG`)**: For very large datums (up to 1GB), chunks form a multi-level directed acyclic tree with intermediate index chunks storing byte offsets and block pointers. This enables fast binary search slicing into arbitrary sub-ranges without reading antecedent chunks.

3. **In-Place Self-Pruning to `LP_UNUSED`**:
   - Because Direct TOAST chunks are not indexed by a B-Tree, when an external datum is deleted or updated, the old TOAST chunks can be pruned immediately during heap pruning or vacuum directly to `LP_UNUSED` (line pointer reuse) without having to wait for index vacuum passes.
   - Controlled by the GUC and storage parameter `direct_toast_self_prune` (default `on`).

4. **In-Place Migration & Compatibility**:
   - Provides `pg_ensure_direct_toast(table_name)` for in-place migration of existing legacy TOAST tables to Direct TOAST format without requiring a table rewrite.
   - Fully integrated into replication, logical decoding, online table reorganization (`pg_repack`), and corruption checking (`amcheck`).
