# What: Pluggable Encoding Types and Contexts

## Overview

The `pg-compression-context` infrastructure introduces pluggable column-level encoding and compression contexts to PostgreSQL core. It generalizes TOAST compression beyond built-in algorithms (pglz, lz4) by introducing full catalog, DDL, and C API abstractions for custom encoders and domain-specific compressors:

![Encoding Pipeline](../img/encoding_cascade_pipeline.svg)

1. **System Catalogs**:
   - **`pg_encoding_type`**: Catalogs available encoding algorithms and compression engines (e.g. `avro`, `zstd`, `lz4`, `cascade`, `bitpack`, `delta`). Stores function pointers for encoding, decoding, validation, and training.
   - **`pg_encoding_context`**: Catalogs specific instantiated contexts configured with user-defined options (e.g., dictionary IDs, compression levels, target chunk sizes, schema definitions, or cascading pipeline stages).

2. **SQL DDL Grammar**:
   - `CREATE ENCODING TYPE name ( ENCODE = enc_fn, DECODE = dec_fn [, VALIDATOR = val_fn] [, OPTIONS = opt_fn] );`
   - `CREATE ENCODING CONTEXT name ( ENCODING TYPE type_name [, OPTIONS ( key 'val', ... )] );`
   - `ALTER TABLE table_name ALTER COLUMN column_name SET ENCODING CONTEXT context_name;`
   - `DROP ENCODING CONTEXT context_name;`
   - `DROP ENCODING TYPE type_name;`

3. **C API & Backend Integration**:
   - Backend cache (`EncodingContextCache`) providing $O(1)$ fast lookup of active contexts and cached function call info.
   - Integration with Direct TOAST: chunks store a `chunk_ectx_id` field, allowing different chunks or different versions of rows to carry their own specific encoding context identifier.
   - Slicing and partial decompression support: encoding context decoders can implement byte-range decoding to decompress only the requested slice of a large attribute.
   - Whole-datum compression support (`compress_method == 3`), enabling compressors that operate across the entire datum before chunking.
