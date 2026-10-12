# Technical Design & Implementation: TOAST Compression Modules

## 1. Extension Modules Implementation Architecture

The compression modules (`contrib/pg_avro`, `contrib/pg_zstd`, `contrib/pg_lz4`, `contrib/pg_cascade`) conform to PostgreSQL's unified encoding context signature:

```c
Datum my_encoding_encode(PG_FUNCTION_ARGS);
Datum my_encoding_decode(PG_FUNCTION_ARGS);
```
- Argument 0: `internal` pointer to `EncodingContextState` containing options, buffer pointers, and destination buffers.
- Argument 1: Datum to encode/decode.
- Returns: `bytea` (for encode) or target type (for decode).

---

## 2. Module Implementations

### 2.1 `contrib/pg_avro`
- Implements Apache Avro ZigZag encoding:
  - Positive integers mapped to $2n$, negative integers mapped to $2|n|-1$.
  - 7-bit variable-length byte chunks with the high bit indicating continuation.
- Reduces positive/negative integer storage from 4 or 8 bytes to 1–3 bytes on average.

### 2.2 `contrib/pg_zstd`
- Links dynamically against `libzstd` or internal bundled implementation.
- Implements `ZSTD_compressCCtx` and `ZSTD_decompressDCtx` with reusable context memory allocations across repeated calls.
- Supports pre-trained compression dictionary caching via internal dictionary registry.

### 2.3 `contrib/pg_lz4`
- Implements `LZ4_compress_default` and `LZ4_decompress_safe`.
- Operates directly on contiguous memory without intermediate allocations, achieving raw throughput exceeding 500 MB/s per core.

### 2.4 `contrib/pg_cascade`
- Implements sequential stage execution:
  ```c
  Datum
  pg_cascade_encode(PG_FUNCTION_ARGS)
  {
      CascadeContext *ctx = (CascadeContext *) PG_GETARG_POINTER(0);
      Datum current = PG_GETARG_DATUM(1);

      for (int i = 0; i < ctx->num_stages; i++)
      {
          current = FunctionCallInvoke(&ctx->stages[i].encode_fn, ...);
      }
      return current;
  }
  ```
- Decoding iterates backward from `num_stages - 1` down to 0, ensuring exact symmetric reconstruction.

---

## 3. Core Engine Direct TOAST Whole-Datum Integration

In `src/backend/access/common/toast_internals.c` and `detoast.c`:
- `compress_method == 3` (Whole-Datum Direct TOAST):
  1. The full unchunked attribute datum is passed to the configured encoding context function.
  2. If the encoded result fits within a single chunk, it is stored as `VARTAG_DIRECT_SINGLE`.
  3. If the encoded result exceeds a single chunk, it is partitioned into Direct TOAST chunks without repeating per-chunk compression headers.
  4. Detoasting reconstructs the contiguous encoded bytea before invoking the decoder.
