# Why: Rationale for Pluggable Encoding Contexts

## Motivation and Industry Context

PostgreSQL natively supports two general-purpose compression algorithms for out-of-line TOAST values: `pglz` (legacy) and `lz4` (modern). While these work well for general unstructured text, modern applications store massive volumes of specialized, highly structured data:

1. **Domain-Specific Compression Gaps**:
   - **JSON / Semi-Structured Documents**: Repetitive key names (`"timestamp"`, `"user_id"`, `"metrics"`) compress poorly under general-purpose LZ block algorithms unless trained dictionaries (e.g. Zstandard dictionaries) are employed.
   - **Time-Series & Sensor Streams**: Monotonically increasing timestamps and numeric sensor readings achieve 10x higher compression when passed through delta encoding, ZigZag integer encoding, or bit-packing before compression.
   - **Vector & Embeddings Data**: High-dimensional vector floats (e.g., 1536-dimensional AI embeddings) do not compress well with standard byte-level LZ compressors; they require specialized scalar or product quantization.

2. **Hardcoded Engine Limitations**:
   Historically in PostgreSQL, adding a new compression algorithm required modifying core catalog files (`pg_type.h`, `toasting.c`, `detoast.c`), editing hardcoded enums, and maintaining a custom server fork. Extensions had no supported mechanism to register custom compressors or custom parameter contexts (e.g., compression dictionaries or per-column quality levels).

## How This Feature Solves the Problem

1. **Pluggable Architecture via Standard DDL**:
   By introducing `pg_encoding_type` and `pg_encoding_context` catalogs, PostgreSQL allows third-party extensions to register custom encoders and compression engines as first-class database objects.
   - Developers can create multiple contexts for the same algorithm with different parameters:
     ```sql
     CREATE ENCODING CONTEXT fast_json (ENCODING TYPE zstd, OPTIONS (level '1'));
     CREATE ENCODING CONTEXT high_ratio_json (ENCODING TYPE zstd, OPTIONS (level '15', dict 'iot_metrics_v2'));
     ```
   - Contexts can be attached per-column (`ALTER TABLE ... SET ENCODING CONTEXT ...`) without affecting other tables.

2. **Fine-Grained Chunk-Level Evolution**:
   By recording `chunk_ectx_id` on physical TOAST chunks:
   - Changing the encoding context of a column does not require rewriting historical table data.
   - Existing rows remain decoded using their historical context ID, while newly inserted rows adopt the new encoding context seamlessly.

3. **Composability via Chained Pipelines (`pg_cascade`)**:
   Enables compounding benefits: structural domain serialization (e.g. `pg_avro`) can be piped directly into high-entropy block compression (e.g. `pg_zstd`), reducing storage footprint by up to **80%–90%**.
