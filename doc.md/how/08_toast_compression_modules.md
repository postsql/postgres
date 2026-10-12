# How: User Guide for TOAST Compression Modules (Avro, Zstandard, LZ4, Cascade)

## 1. Extension Installation

Install the extensions in your target database:

```sql
CREATE EXTENSION pg_avro;
CREATE EXTENSION pg_zstd;
CREATE EXTENSION pg_lz4;
CREATE EXTENSION pg_cascade;
```

---

## 2. Using `pg_avro` for Variable-Length Integer & Structural Encoding

Avro serialization translates standard numeric and structural datums into compact ZigZag and variable-length integer representations.

### Creating an Avro Encoding Context
```sql
CREATE ENCODING CONTEXT ctx_avro_compact (
    ENCODING TYPE avro
);

CREATE TABLE sensor_readings (
    sensor_id INT,
    readings INT8[]
);

ALTER TABLE sensor_readings
ALTER COLUMN readings SET ENCODING CONTEXT ctx_avro_compact;
```

---

## 3. Using `pg_zstd` with Tunable Compression Levels

Zstandard provides superior compression ratios, especially on repetitive document structures.

### Creating Zstandard Contexts
```sql
-- Fast compression for active transactional tables
CREATE ENCODING CONTEXT ctx_zstd_fast (
    ENCODING TYPE zstd,
    OPTIONS (level '1')
);

-- High-ratio compression for cold archives
CREATE ENCODING CONTEXT ctx_zstd_max (
    ENCODING TYPE zstd,
    OPTIONS (level '19')
);

CREATE TABLE customer_contracts (
    contract_id BIGINT PRIMARY KEY,
    signed_pdf_raw BYTEA
);

ALTER TABLE customer_contracts
ALTER COLUMN signed_pdf_raw SET ENCODING CONTEXT ctx_zstd_max;
```

---

## 4. Using `pg_lz4` for Maximum Throughput

LZ4 is optimal when CPU utilization must be minimal and compression throughput is paramount.

```sql
CREATE ENCODING CONTEXT ctx_lz4_realtime (
    ENCODING TYPE lz4,
    OPTIONS (acceleration '1')
);

CREATE TABLE live_market_feed (
    tick_id BIGSERIAL,
    market_snapshot JSONB
);

ALTER TABLE live_market_feed
ALTER COLUMN market_snapshot SET ENCODING CONTEXT ctx_lz4_realtime;
```

---

## 5. Using `pg_cascade` for Multi-Stage Transformation Pipelines

The `pg_cascade` extension chains stages together into an ordered transformation pipeline.

### Example: Combining Avro Structural Encoding with Zstandard Compression
```sql
-- Stage 1 applies Avro ZigZag varint encoding
-- Stage 2 applies Zstandard block compression
CREATE ENCODING CONTEXT ctx_avro_then_zstd (
    ENCODING TYPE cascade,
    OPTIONS (
        stage1 'avro',
        stage2 'zstd'
    )
);

CREATE TABLE telemetry_streams (
    device_id INT,
    recorded_at TIMESTAMPTZ,
    data_points INT8[]
);

ALTER TABLE telemetry_streams
ALTER COLUMN data_points SET ENCODING CONTEXT ctx_avro_then_zstd;
```

### Transparent Verification
```sql
-- Insert large array of integers
INSERT INTO telemetry_streams (device_id, recorded_at, data_points)
SELECT 1, now(), array_agg(i * 100)
FROM generate_series(1, 5000) i;

-- Query the data normally: decompression happens completely transparently
SELECT device_id, array_length(data_points, 1), data_points[1:5]
FROM telemetry_streams;
```

### Inspecting Storage Savings
Compare the on-disk storage footprint using standard PostgreSQL size functions:
```sql
SELECT
    pg_size_pretty(pg_relation_size('telemetry_streams')) AS table_size,
    pg_size_pretty(pg_total_relation_size(reltoastrelid)) AS toast_size
FROM pg_class
WHERE relname = 'telemetry_streams';
```
The cascading combination typically yields an **80%–90% reduction** in physical TOAST table size compared to uncompressed storage.
