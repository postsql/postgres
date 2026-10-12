# How: User Guide for Pluggable Encoding Contexts

## 1. Concepts and Lifecycle

The encoding context framework operates in three simple layers:
1. **Encoding Type (`CREATE ENCODING TYPE`)**: The underlying compression/encoding algorithm, typically provided by an extension (e.g. `pg_avro`, `pg_zstd`, `pg_lz4`, `pg_cascade`).
2. **Encoding Context (`CREATE ENCODING CONTEXT`)**: A configured instance of an encoding type with specific options (e.g. compression level, dictionary ID, stage pipelines).
3. **Column Assignment (`ALTER TABLE ... SET ENCODING CONTEXT`)**: Binding the context to a specific table column.

---

## 2. Setting Up Encoding Contexts

### Step 1: Install Desired Compression Extensions
```sql
CREATE EXTENSION pg_avro;
CREATE EXTENSION pg_zstd;
CREATE EXTENSION pg_lz4;
CREATE EXTENSION pg_cascade;
```

### Step 2: Create Custom Contexts
Create contexts tailored to specific application requirements:

```sql
-- Fast compression for latency-sensitive payloads
CREATE ENCODING CONTEXT ctx_fast_lz4 (
    ENCODING TYPE lz4,
    OPTIONS (level '1')
);

-- High-ratio compression for archived documents
CREATE ENCODING CONTEXT ctx_archive_zstd (
    ENCODING TYPE zstd,
    OPTIONS (level '9')
);

-- Multi-stage cascading pipeline: Avro structural encoding -> Zstandard compression
CREATE ENCODING CONTEXT ctx_avro_zstd (
    ENCODING TYPE cascade,
    OPTIONS (stage1 'avro', stage2 'zstd')
);
```

### Step 3: Attach Encoding Context to Table Column
```sql
CREATE TABLE event_logs (
    event_id BIGINT PRIMARY KEY,
    created_at TIMESTAMPTZ,
    raw_payload JSONB
);

-- Set encoding context for the JSONB payload column
ALTER TABLE event_logs
ALTER COLUMN raw_payload SET ENCODING CONTEXT ctx_avro_zstd;
```

---

## 3. Querying and Managing Catalogs

### Inspecting Configured Contexts
Query the `pg_encoding_context` and `pg_encoding_type` system catalogs:

```sql
SELECT
    ec.ectxname AS context_name,
    et.etypname AS encoding_type,
    ec.ectxoptions AS options
FROM pg_encoding_context ec
JOIN pg_encoding_type et ON ec.ectxtype = et.oid;
```

### Modifying Column Encoding Context
You can switch the encoding context of a column at any time without table rewrite:
```sql
ALTER TABLE event_logs
ALTER COLUMN raw_payload SET ENCODING CONTEXT ctx_archive_zstd;
```
- Existing rows stored with `ctx_avro_zstd` remain decodable using their stored chunk context ID.
- New rows will be written with `ctx_archive_zstd`.

### Removing an Encoding Context
```sql
-- Detach from column first
ALTER TABLE event_logs ALTER COLUMN raw_payload RESET ENCODING CONTEXT;

-- Drop context
DROP ENCODING CONTEXT ctx_avro_zstd;
```
