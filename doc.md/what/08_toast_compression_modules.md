# What: TOAST Compression Modules (Avro, Zstandard, LZ4, Cascade)

## Overview

This development introduces a modular suite of contrib extensions implementing advanced serialization, dictionary compression, and multi-stage pipeline processing for out-of-line TOAST values and encoding contexts:

1. **`contrib/pg_avro` (Avro Varint & Structural Serialization)**:
   - Implements Apache Avro variable-length integer encoding (ZigZag and variable-byte encoding) for integers, floats, timestamps, and serialized structures.
   - Provides functions `pg_avro.encode(internal, anyelement) RETURNS bytea` and `pg_avro.decode(internal, bytea) RETURNS anyelement`.
   - Shrinks sparse integer and structured data sizes significantly before general compression algorithms run.

2. **`contrib/pg_zstd` (Zstandard Compression with Dictionary Support)**:
   - Wraps modern Zstandard compression algorithms into PostgreSQL encoding contexts.
   - Supports pre-trained compression dictionaries, yielding 3x–10x higher compression ratios on repetitive JSON, XML, or structured payloads compared to default `pglz` or `lz4`.
   - Exposes `pg_zstd.compress` and `pg_zstd.decompress` functions, configurable with compression levels and dictionary IDs via context options.

3. **`contrib/pg_lz4` (LZ4 High-Throughput Block Compression)**:
   - Exposes raw LZ4 block compression routines as pluggable encoding context routines.
   - Designed for high-throughput, latency-critical read/write workloads where CPU overhead must be minimized.
   - Exposes `pg_lz4.compress` and `pg_lz4.decompress` with support for whole-datum and chunk-level execution.

4. **`contrib/pg_cascade` (Multi-Stage Cascading Pipeline)**:
   - A meta-encoding extension that chains multiple encoding or compression stages together in sequence:
     $$\text{Raw Data} \xrightarrow{\text{Stage 1 (e.g. Avro / Delta)}} \text{Intermediate} \xrightarrow{\text{Stage 2 (e.g. Zstandard)}} \text{Compressed Chunk}$$
   - During decoding, operations are applied in exact reverse order.
   - Configurable dynamically via context options: `OPTIONS (stage1 'avro', stage2 'zstd')`.

5. **Core Engine Integration**:
   - GUC `direct_toast_chunk_compression` (default `on`): Controls whether Direct TOAST applies chunk compression.
   - Whole-datum compression engine (`compress_method == 3`): Compresses the full unchunked datum before splitting into Direct TOAST chunks.
   - Dynamic runtime resolver (`resolve_extension_encoding_func`): Resolves extension-provided C functions dynamically on first use and caches function pointers in shared memory.
