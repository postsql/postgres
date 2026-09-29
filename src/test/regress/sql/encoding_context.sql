--
-- ENCODING_CONTEXT
-- Test pg_encoding_type and pg_encoding_context catalogs, C API user
-- functions, composable multi-stage pipelines, backend-local cache
-- invalidation, dependencies, and privilege enforcement.
--

-- Verify catalog schemas exist and are initially empty
SELECT count(*) FROM pg_encoding_type;
SELECT count(*) FROM pg_encoding_context;

-- 1. Create encoding types
-- Reject invalid enctypkind
SELECT pg_create_encoding_type('bad_kind_type', 'x', 'custom',
                               'pg_bytea_dict_encode', 'pg_bytea_dict_decode');

-- Register dictionary encoding type ('s' = serialization/dictionary) and pglz ('c' = compression)
SELECT pg_create_encoding_type('test_dict_type', 's', 'xor_lut',
                               'pg_bytea_dict_encode', 'pg_bytea_dict_decode') IS NOT NULL AS created_dict_type;

SELECT pg_create_encoding_type('test_pglz_type', 'c', 'pglz',
                               'pg_pglz_context_encode', 'pg_pglz_context_decode') IS NOT NULL AS created_pglz_type;

-- Duplicate encoding type name in same schema must fail
SELECT pg_create_encoding_type('test_dict_type', 's', 'xor_lut',
                               'pg_bytea_dict_encode', 'pg_bytea_dict_decode');

SELECT enctypname, enctypkind, enctyptype, enctypencfunc, enctypdecfunc
FROM pg_encoding_type
ORDER BY enctypname;

-- 2. Single-stage encoding contexts: symmetric, asymmetric 256-byte LUT, and 0-byte/NULL edge cases
-- 2a. Symmetric dictionary (dectab = NULL falls back to enctab)
SELECT pg_create_encoding_context(
    'ctx_sym_text',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    0,
    'text'::regtype,
    '\x5a3c99f0'::bytea,
    NULL,
    'v1-sym',
    ARRAY['mode=xor']
) IS NOT NULL AS created_sym_ctx;

SELECT ectxname, ectxparentid, ectxtargettypid::regtype, ectxlabel,
       encode(ectxenctab, 'hex') AS enctab_hex,
       ectxdectab IS NULL AS dectab_is_null,
       ectxoptions
FROM pg_encoding_context
WHERE ectxname = 'ctx_sym_text';

-- Encode and decode round-trip with symmetric dictionary
SELECT encode(pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    'Hello PostgreSQL Encoding Context!'::text
), 'hex') AS sym_encoded_hex;

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
        'Hello PostgreSQL Encoding Context!'::text
    ),
    NULL::text
) AS sym_decoded_text;

-- 2b. Asymmetric 256-byte lookup table (exercises enc_opaque / dec_opaque caching)
-- Build a 256-byte Caesar shift (+13 mod 256) enctab and (-13 mod 256) dectab
SELECT pg_create_encoding_context(
    'ctx_asym_lut',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    0,
    'text'::regtype,
    (SELECT string_agg(decode(lpad(to_hex((i + 13) % 256), 2, '0'), 'hex'), ''::bytea ORDER BY i)
     FROM generate_series(0, 255) i),
    (SELECT string_agg(decode(lpad(to_hex((i + 256 - 13) % 256), 2, '0'), 'hex'), ''::bytea ORDER BY i)
     FROM generate_series(0, 255) i),
    'v1-asym-lut',
    NULL
) IS NOT NULL AS created_asym_ctx;

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_asym_lut'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_asym_lut'),
        'Asymmetric 256-byte LUT test payload'::text
    ),
    NULL::text
) AS asym_decoded_text;

-- Verify encoded payload actually differs from plaintext
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_asym_lut'),
    'Asymmetric 256-byte LUT test payload'::text
) <> convert_to('Asymmetric 256-byte LUT test payload', 'UTF8') AS asym_transformed;

-- 2c. Edge case: NULL enctab and 0-byte enctab
SELECT pg_create_encoding_context(
    'ctx_empty_tab',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    0,
    'bytea'::regtype,
    ''::bytea
) IS NOT NULL AS created_empty_ctx;

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_empty_tab'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_empty_tab'),
        '\xdeadbeef'::bytea
    ),
    NULL::bytea
) AS empty_tab_roundtrip;

-- 3. Composable multi-stage encoding context pipeline
-- Child context with non-bytea target_typid must be rejected
SELECT pg_create_encoding_context(
    'ctx_bad_child_type',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type'),
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    'text'::regtype
);

-- Create valid child PGLZ compression stage chained onto root ctx_sym_text
-- Passing target_typid = 0 defaults child stage target type to bytea
SELECT pg_create_encoding_context(
    'ctx_chained_dict_pglz',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type'),
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    0,
    NULL,
    NULL,
    'v1-dict-plus-pglz',
    ARRAY['level=default']
) IS NOT NULL AS created_chained_ctx;

SELECT c.ectxname,
       p.ectxname AS parent_name,
       c.ectxtargettypid::regtype AS child_target_type,
       p.ectxtargettypid::regtype AS root_target_type
FROM pg_encoding_context c
JOIN pg_encoding_context p ON p.oid = c.ectxparentid
WHERE c.ectxname = 'ctx_chained_dict_pglz';

-- Verify composable execution order:
-- Encode: pglz_encode(dict_encode(val))
-- Decode: dict_decode(pglz_decode(raw))
WITH sample AS (
    SELECT repeat('{"sensor":"temp","val":23.5,"unit":"C"}', 20) AS msg
),
encoded AS (
    SELECT msg,
           pg_encode_datum(
               (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
               msg
           ) AS comp_bytes
    FROM sample
)
SELECT octet_length(comp_bytes) < octet_length(msg) AS is_compressed,
       pg_decode_datum(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
           comp_bytes,
           NULL::text
       ) = msg AS composable_roundtrip_ok
FROM encoded;

-- Verify pg_column_raw_encoding extracts raw bytes without error
SELECT encode(pg_column_raw_encoding('raw_payload'::text), 'escape') AS raw_extract;
SELECT length(pg_column_raw_encoding(12345678::int4)) AS int4_raw_len;

-- 4. Cascading cache invalidation when parent context is altered
-- Alter the parent context (ctx_sym_text) dictionary and verify that
-- decoding/encoding via the child context (ctx_chained_dict_pglz) immediately
-- reflects the updated parent dictionary.
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
    'Invalidate parent test'::text
) AS before_alter_bytes \gset

SELECT pg_alter_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    '\x11223344'::bytea,
    NULL,
    'v2-sym-updated',
    ARRAY['mode=xor-v2']
);

SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
    'Invalidate parent test'::text
) <> :'before_alter_bytes'::bytea AS child_cache_invalidated;

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
        'Invalidate parent test'::text
    ),
    NULL::text
) AS roundtrip_after_parent_alter;

-- 5. Max pipeline depth guard (MAX_ENCODING_CONTEXT_DEPTH = 8)
DO $$
DECLARE
    t_oid oid;
    cur_parent oid;
    i int;
BEGIN
    SELECT oid INTO t_oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type';
    SELECT oid INTO cur_parent FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz';
    -- ctx_sym_text (1) -> ctx_chained_dict_pglz (2). Add stages 3..8 (should succeed)
    FOR i IN 3..8 LOOP
        cur_parent := pg_create_encoding_context('ctx_depth_' || i, t_oid, cur_parent);
    END LOOP;
    -- Stage 9 must fail with depth limit exceeded
    BEGIN
        PERFORM pg_create_encoding_context('ctx_depth_9', t_oid, cur_parent);
        RAISE EXCEPTION 'expected depth limit error did not occur';
    EXCEPTION WHEN program_limit_exceeded THEN
        RAISE NOTICE 'caught expected depth limit error: %', sqlerrm;
    END;
END;
$$;

-- 6. Column binding (pg_set_column_encoding_context) and dependency tracking
CREATE TABLE test_ectx_tbl (
    id int,
    payload text
);

-- Type mismatch: ctx_chained_dict_pglz has root target type 'text', cannot bind to 'id' (int4)
SELECT pg_set_column_encoding_context(
    'test_ectx_tbl',
    'id',
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz')
);

-- Valid binding to 'payload' (text)
SELECT pg_set_column_encoding_context(
    'test_ectx_tbl',
    'payload',
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz')
);

SELECT a.attname, c.ectxname
FROM pg_attribute a
JOIN pg_encoding_context c ON c.oid = a.attencoding_context
WHERE a.attrelid = 'test_ectx_tbl'::regclass;

-- Dropping referenced context or parent context with RESTRICT (cascade = false) must fail
SELECT pg_drop_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_chained_dict_pglz'),
    false
);

SELECT pg_drop_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    false
);

-- Unbind column encoding context by passing 0
SELECT pg_set_column_encoding_context('test_ectx_tbl', 'payload', 0);

SELECT attencoding_context FROM pg_attribute
WHERE attrelid = 'test_ectx_tbl'::regclass AND attname = 'payload';

DROP TABLE test_ectx_tbl;

-- 7. Security & privilege tests (unprivileged user must be denied admin functions)
CREATE ROLE regress_encoding_user;
SET ROLE regress_encoding_user;

SELECT pg_create_encoding_type('unpriv_type', 'c', 'pglz',
                               'pg_pglz_context_encode', 'pg_pglz_context_decode');
SELECT pg_drop_encoding_type(
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type'),
    false
);
SELECT pg_create_encoding_context(
    'unpriv_ctx',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type')
);
SELECT pg_alter_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    '\x00'::bytea
);
SELECT pg_drop_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    false
);

RESET ROLE;
DROP ROLE regress_encoding_user;

-- 8. Cleanup via CASCADE
SELECT pg_drop_encoding_type(
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    true
);
SELECT pg_drop_encoding_type(
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type'),
    true
);

SELECT count(*) FROM pg_encoding_type;
SELECT count(*) FROM pg_encoding_context;
