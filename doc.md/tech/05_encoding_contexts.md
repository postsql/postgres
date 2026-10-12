# Technical Design & Implementation: Encoding Contexts

## 1. System Catalogs & OID Usage

The compression and encoding context infrastructure is integrated into PostgreSQL's core catalog architecture via three distinct catalog layers:

```text
┌─────────────────────────────────────────────────────────────┐
│                    pg_encoding_type                         │
│  (oid, etypname, etypencode, etypdecode, etypvalidator, ..) │
└──────────────────────────────┬──────────────────────────────┘
                               │ 1 : N
┌──────────────────────────────▼──────────────────────────────┐
│                   pg_encoding_context                       │
│  (oid, ectxname, ectxtype, ectxparent, ectxoptions, ...)    │
└───────────────▲──────────────────────────────▲──────────────┘
                │ references (OID)             │ references (OID)
┌───────────────┴───────────────┐ ┌────────────┴──────────────┐
│         pg_attribute          │ │    Direct TOAST Table     │
│   (attencoding_context OID)   │ │    (chunk_ectx_id OID)    │
│  Default context for new data │ │ Self-describing data chunk│
└───────────────────────────────┘ └───────────────────────────┘
```

### 1.1 `pg_encoding_type` Catalog
Defined in `src/include/catalog/pg_encoding_type.h`. Catalogs available algorithms and compression engines:
- **`oid`** (`Oid`): Unique identifier of the encoding algorithm.
- **`etypname`** (`NameData`): Algorithm identifier (`avro`, `zstd`, `lz4`, `cascade`, etc.).
- **`etypencode`** (`regproc`): C function OID implementing the compression/encoding routine.
- **`etypdecode`** (`regproc`): C function OID implementing the decompression/decoding routine.
- **`etypvalidator`** (`regproc`): Optional validator function checking option keys and values.
- **`etypoptions`** (`regproc`): Optional parser function transforming text options into C structs.

### 1.2 `pg_encoding_context` Catalog
Defined in `src/include/catalog/pg_encoding_context.h`. Catalogs instantiated, parameterized contexts:
- **`oid`** (`Oid`): **The Compression Context OID**. Unique primary key for each context instance.
  - Indexed via `pg_encoding_context_oid_index` (OID primary key) and `pg_encoding_context_name_nsp_index` (`(ectxname, ectxnamespace)`).
- **`ectxname`** (`NameData`): User-visible context name.
- **`ectxnamespace`** (`Oid`): Schema namespace owning this context.
- **`ectxowner`** (`Oid`): Role OID owning this context.
- **`ectxtype`** (`Oid`): Foreign key referencing `pg_encoding_type.oid`.
- **`ectxparent`** (`Oid`): Foreign key referencing another `pg_encoding_context.oid`. Used to build hierarchical or multi-stage encoding chains.
- **`ectxtargettype`** (`Oid`): Foreign key referencing `pg_type.oid` when a context specifically targets a specific SQL data type.
- **`ectxlevel`** (`int2`): Hierarchy depth in cascade chains.
- **`ectxenctab`** (`bytea`): Pre-trained encoding substitution table or dictionary payload.
- **`ectxdectab`** (`bytea`): Pre-trained decoding substitution table or dictionary payload.
- **`ectxlabel`** (`text`): Descriptive label or documentation string.
- **`ectxoptions`** (`text[]`): Key-value option array (`stage1=avro`, `level=9`, etc.).

### 1.3 `pg_attribute.attencoding_context`
In `src/include/catalog/pg_attribute.h`, the core column catalog `FormData_pg_attribute` includes:
```c
Oid attencoding_context BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_encoding_context);
```
- **Role**: Stores the `pg_encoding_context.oid` currently assigned to that table column.
- **Setting**: Executed via `ALTER TABLE tbl ALTER COLUMN col SET ENCODING CONTEXT ctx_name;`. `ColumnSetEncodingContext()` resolves `ctx_name` to its OID, validates datatype compatibility (`GetRootEncodingContextTargetType`), and stores the context OID in `attencoding_context`.
- **Relcache Caching**: Cached in the column descriptor `att->attencoding_context` in `Relation->rd_att`. During heap tuple insertion, `toast_tuple_init()` reads `att->attencoding_context` and passes it to `ttc_attr[i].tai_encoding_context`.
- **TOAST Upgrade**: Setting `attencoding_context` on a column automatically invokes `ensure_direct_toast_ectx(relid)` to ensure the underlying TOAST table is in Direct TOAST format with the 6-column schema.

### 1.4 `pg_depend` Object Dependency Tracking
- When a column is configured with an encoding context, `ColumnSetEncodingContext()` creates a dependency record in `pg_depend` between:
  - Class: `RelationRelationId` (`pg_class`)
  - Object: `relid`
  - SubObject: `attnum` (the specific column)
  - Referenced Class: `EncodingContextRelationId` (`pg_encoding_context`)
  - Referenced Object: `context_id` (the context OID)
  - Type: `DEPENDENCY_NORMAL`
- This ensures that attempting to execute `DROP ENCODING CONTEXT ctx_name;` while a table column still uses it is rejected with a clean dependency error, unless `CASCADE` is explicitly specified.
- The context itself records dependencies on its namespace, owner, `pg_encoding_type`, parent context, and target SQL datatype.

---

## 2. Encoded Data Representation: The `chunk_ectx_id` Architecture

A central design principle of the encoding context system is that **encoded data is completely self-describing**. The context OID is persisted alongside the compressed data itself rather than relying solely on catalog state.

### 2.1 Direct TOAST 6-Column Schema
When Direct TOAST supports encoding contexts, the TOAST table (`pg_toast_<oid>`) has 6 columns:
1. `chunk_id` (`INT4OID` / `OIDOID`): NULL for Direct TOAST tuples (kept for legacy Plain TOAST compatibility).
2. `chunk_seq` (`INT4OID`): Sequence index for ordering.
3. `chunk_data` (`BYTEAOID`): The raw or compressed chunk payload.
4. `chunk_tids` (`TIDARRAYOID`): Array of child TIDs for flat arrays and tree roots.
5. `chunk_tid_offsets` (`INT8ARRAYOID`): Cumulative byte offset boundaries for DAG binary search.
6. **`chunk_ectx_id` (`OIDOID`)**: **The Compression Context OID** used to encode this specific chunk.

### 2.2 Writing Chunks with Context OID
In `src/backend/access/common/toast_internals.c` (`toast_direct_insert_chunk`):
```c
if (state->ectx != NULL && (chunk_data_p != NULL || !state->encode_per_chunk))
{
    /* Store the pg_encoding_context OID in column 6 */
    t_values[5] = ObjectIdGetDatum(state->ectx->context_id);
    t_isnull[5] = false;
}
else
{
    t_values[5] = (Datum) 0;
    t_isnull[5] = true;
}

toasttup = heap_form_tuple(state->toasttupDesc, t_values, t_isnull);
heap_insert(state->toastrel, toasttup, state->mycid, state->options, NULL);
```

### 2.3 Reading & Decoding Chunks
During detoasting in `src/backend/access/common/detoast.c`:
1. When a chunk tuple is fetched from the TOAST relation:
   ```c
   if (toastrel->rd_att->natts >= 6)
   {
       bool is_null_ectx;
       Datum ectx_datum = fastgetattr(&tup, 6, toastrel->rd_att, &is_null_ectx);
       if (!is_null_ectx)
           chunk_ectx_id = DatumGetObjectId(ectx_datum);
   }
   ```
2. If `chunk_ectx_id` is valid (`OidIsValid(chunk_ectx_id)`):
   ```c
   decoded = toast_decode_chunk_with_context(chunk_ectx_id, raw_copy);
   ```
3. `toast_decode_chunk_with_context()` looks up `GetEncodingContext(chunk_ectx_id)` in the backend cache in $O(1)$ and executes the decoder function.
4. If `chunk_ectx_id` is NULL / invalid, the chunk is treated as standard uncompressed/raw bytes.

---

## 3. Whole-Datum vs. Per-Chunk Encoding

The architecture supports two distinct execution models:

### 3.1 Per-Chunk Mode (`direct_toast_chunk_compression = on`)
- Used when chunks are compressed individually.
- Each chunk in the TOAST table is independently encoded and tagged with its own `chunk_ectx_id`.
- Slicing and partial range reads only need to decompress the specific chunk(s) covering the requested byte range.

### 3.2 Whole-Datum Mode (`compress_method == 3`)
- Used when `EncodingContextIsWholeDatum(ectx, valtype)` is true (e.g. structural encoders like Avro, or whole-payload dictionary compression).
- The entire datum is encoded as a single continuous byte stream before chunking.
- The external pointer's `va_extinfo` field sets bits `(3U << VARLENA_EXTSIZE_BITS)`:
  ```c
  if (whole_encoded != NULL)
      toast_pointer.va_extinfo |= (3U << VARLENA_EXTSIZE_BITS);
  ```
- All chunks written for this datum record the context OID in `chunk_ectx_id`.
- During detoasting, the chunks are reassembled into the complete encoded bytea, and the `chunk_ectx_id` from the root/first chunk is used to decode the entire datum in one pass.

---

## 4. Architectural Advantages of Self-Describing Data

Storing the compression context OID in the encoded data itself (`chunk_ectx_id`) provides critical operational advantages:

1. **Zero-Downtime Context Switching (No Table Rewrite Required)**:
   - When an administrator runs:
     ```sql
     ALTER TABLE events ALTER COLUMN payload SET ENCODING CONTEXT ctx_v2;
     ```
     `pg_attribute.attencoding_context` is updated to point to `ctx_v2`.
   - **Existing rows are not rewritten**. Existing chunks still have `chunk_ectx_id = <oid_of_ctx_v1>`.
   - When existing rows are read, `detoast.c` reads the chunk's stored `chunk_ectx_id`, fetches `ctx_v1`, and decodes the row flawlessly.
   - Newly inserted or updated rows are compressed with `ctx_v2` and store `chunk_ectx_id = <oid_of_ctx_v2>`.

2. **Heterogeneous Coexistence**:
   - A single TOAST table can concurrently contain chunks compressed with different algorithms (e.g. legacy chunks with `pglz`, intermediate chunks with `pg_lz4`, and modern chunks with `pg_cascade`).
   - Each chunk knows exactly which context OID and decoder is required to read it.

3. **Replication and Logical Decoding Safety**:
   - Logical replication subscribers and decoding plugins receive the self-describing context OID on the chunks, ensuring identical decoding behavior regardless of subscriber catalog defaults.

---

## 5. Dynamic Function Resolution & Backend Cache

In `src/backend/access/common/encoding_context.c`:

### 5.1 Backend Cache (`EncodingContextCache`)
A memory-context-local hash table maps context OIDs to cached `EncodingContextData` structures:
```c
typedef struct CachedEncodingContext {
    Oid         ectx_oid;
    Oid         etyp_oid;
    FmgrInfo    encode_flinfo;
    FmgrInfo    decode_flinfo;
    void       *parsed_options;
    bool        is_valid;
} CachedEncodingContext;
```
Lookups complete in $O(1)$ without repetitive catalog scans or lock acquisitions. When an `ALTER ENCODING CONTEXT` or `DROP ENCODING CONTEXT` occurs, relcache invalidation messages clear the cached entry via `InvalidateEncodingContext()`.

### 5.2 Dynamic Extension Function Resolver (`resolve_extension_encoding_func`)
When an encoding type is provided by a dynamically loaded shared library (`.so`), the resolver binds the C symbols dynamically on first invocation and caches the function pointer directly in the context struct.
