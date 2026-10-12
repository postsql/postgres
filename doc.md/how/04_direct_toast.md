# How: User Guide for Direct TOAST

## 1. Configuring Direct TOAST

Direct TOAST is controlled via server GUC configuration or per-table storage parameters.

### Server-Wide Default (`postgresql.conf`)
```ini
# Options: 'plain' (legacy OID-indexed) or 'direct' (direct TID-addressed)
toast_flavour = direct

# Enable or disable immediate in-place pruning of unindexed dead chunks
direct_toast_self_prune = on
```

### Session-Level Configuration (SQL)
```sql
-- Check current flavour
SHOW toast_flavour;

-- Set default for new tables created in this session
SET toast_flavour = 'direct';
```

---

## 2. Table-Level Storage Parameters

You can choose the TOAST flavour per table using table storage options:

```sql
-- Create a table using Direct TOAST
CREATE TABLE documents (
    id BIGINT PRIMARY KEY,
    title TEXT,
    body TEXT
) WITH (toast_flavour = 'direct', direct_toast_self_prune = true);

-- Create a table explicitly using legacy Plain TOAST
CREATE TABLE legacy_docs (
    id BIGINT PRIMARY KEY,
    payload BYTEA
) WITH (toast_flavour = 'plain');
```

---

## 3. In-Place Migration of Existing Tables

To upgrade an existing Plain TOAST table to Direct TOAST format without a table rewrite or table lock downtime:

```sql
-- Upgrades the TOAST table catalog and format in-place
SELECT pg_ensure_direct_toast('documents');
```

After calling `pg_ensure_direct_toast()`:
- Existing rows remain readable through the unified detoasting pipeline.
- Newly written or updated out-of-line attributes are automatically written in Direct TOAST format.

---

## 4. Verifying Direct TOAST Tables with `amcheck`

The `contrib/amcheck` extension includes full verification for Direct TOAST tuples, validating both single chunks, flat arrays, and hierarchical DAG structures:

```sql
CREATE EXTENSION amcheck;

-- Verify heap and all associated Direct TOAST chunks
SELECT verify_heapam('documents');
```

---

## 5. Performance Tips and Best Practices

1. **Large Document Stores (JSONB, XML, PDF, Text)**:
   For tables storing multi-megabyte JSONB or text blobs, Direct TOAST provides immediate speedups:
   - 2x–5x faster reads because B-Tree index lookups are bypassed.
   - Slicing sub-ranges (`substr()`, `get_byte()`) uses the hierarchical DAG for fast sub-range retrieval.
2. **High-Churn Update Tables**:
   Keep `direct_toast_self_prune = on`. When large rows are updated frequently, old chunks are reclaimed immediately on the page as `LP_UNUSED`, completely avoiding the explosive TOAST table bloat typical of Plain TOAST.
