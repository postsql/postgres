--
-- ENCODING_CONTEXT
-- Test pg_encoding_type and pg_encoding_context catalogs, C API user
-- functions, context groups (ectxgroup and ectxvariant), lazy fallback
-- resolution, common name/OID resolution, backend-local cache invalidation,
-- dependencies, and privilege enforcement.
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

-- 2. Standalone encoding contexts (ectxvariant = 0, ectxgroup = oid):
--    symmetric, asymmetric 256-byte LUT, PGLZ, and 0-byte/NULL edge cases
-- 2a. Symmetric dictionary (dectab = NULL falls back to enctab)
SELECT pg_create_encoding_context(
    'ctx_sym_text',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype,
    '\x5a3c99f0'::bytea,
    NULL,
    'v1-sym',
    ARRAY['mode=xor']
) IS NOT NULL AS created_sym_ctx;

SELECT ectxname,
       ectxgroup = oid AS group_equals_oid,
       ectxvariant,
       ectxtargettypid::regtype,
       ectxlabel,
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

-- 2d. Standalone PGLZ compression context
SELECT pg_create_encoding_context(
    'ctx_pglz_text',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_pglz_type'),
    'text'::regtype,
    NULL,
    NULL,
    'v1-pglz',
    ARRAY['level=default']
) IS NOT NULL AS created_pglz_ctx;

WITH sample AS (
    SELECT repeat('{"sensor":"temp","val":23.5,"unit":"C"}', 20) AS msg
),
encoded AS (
    SELECT msg,
           pg_encode_datum(
               (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_pglz_text'),
               msg
           ) AS comp_bytes
    FROM sample
)
SELECT octet_length(comp_bytes) < octet_length(msg) AS is_compressed,
       pg_decode_datum(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_pglz_text'),
           comp_bytes,
           NULL::text
       ) = msg AS pglz_roundtrip_ok
FROM encoded;

-- Verify pg_column_raw_encoding extracts raw bytes without error
SELECT encode(pg_column_raw_encoding('raw_payload'::text), 'escape') AS raw_extract;
SELECT length(pg_column_raw_encoding(12345678::int4)) AS int4_raw_len;

-- 3. Context Groups (ectxgroup & ectxvariant), Lazy Fallback Resolution,
--    and Common Name/OID Resolver (pg_resolve_encoding_context_oid)
-- 3a. Group leader (variant = 1, group_id = 0 -> ectxgroup = oid): restricted digits alphabet
SELECT pg_create_encoding_context(
    'ctx_grp_digits',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype,
    '0123456789'::bytea,
    '9876543210'::bytea,
    'grp-v1-digits',
    NULL,
    true,
    0,
    1
) IS NOT NULL AS created_grp_leader;

-- 3b. Higher variant in the same group (variant = 5, group_id = ctx_grp_digits): general XOR
SELECT pg_create_encoding_context(
    'ctx_grp_xor_v5',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype,
    '\x5a3c99f0'::bytea,
    '\x5a3c99f0'::bytea,
    'grp-v5-xor',
    NULL,
    true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    5
) IS NOT NULL AS created_grp_v5;

SELECT c.ectxname,
       g.ectxname AS group_leader_name,
       c.ectxgroup = g.oid AS in_leader_group,
       c.ectxvariant,
       c.ectxtargettypid::regtype AS target_type
FROM pg_encoding_context c
JOIN pg_encoding_context g ON g.oid = c.ectxgroup
WHERE c.ectxname IN ('ctx_grp_digits', 'ctx_grp_xor_v5')
ORDER BY c.ectxvariant;

-- 3c. Common name-or-OID resolver (pg_resolve_encoding_context_oid)
SELECT pg_resolve_encoding_context_oid('ctx_grp_digits') =
       (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits') AS resolved_by_name,
       pg_resolve_encoding_context_oid('public.ctx_grp_digits') =
       (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits') AS resolved_by_qualname,
       pg_resolve_encoding_context_oid((SELECT oid::text FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits')) =
       (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits') AS resolved_by_numeric_oid,
       pg_resolve_encoding_context_oid('non_existent_ctx', true) = 0 AS missing_ok_returns_zero;

-- Non-existent context with missing_ok = false must raise error
SELECT pg_resolve_encoding_context_oid('non_existent_ctx', false);

-- 3d. Lazy fallback resolution (ECTX_UNRESOLVED -> variant 5):
-- Pure digits succeed on variant 1 (ctx_grp_digits)
SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
        '0123456789'::text
    ),
    NULL::text
) AS grp_digits_roundtrip;

-- Non-digit characters ('0123abc') fail variant 1 and lazily resolve & fall back to variant 5 (ctx_grp_xor_v5)
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    '0123abc'::text
) = pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_xor_v5'),
    '0123abc'::text
) AS fell_back_to_variant_5;

-- 4. Dynamic variant insertion & cache invalidation
-- Insert an intermediate variant (variant = 3) into ctx_grp_digits's group and
-- verify that encoding '0123abc' via ctx_grp_digits (variant 1) immediately
-- invalidates the cached fallback pointer and falls back to variant 3 instead of variant 5!
SELECT pg_create_encoding_context(
    'ctx_grp_xor_v3',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype,
    '\x11223344'::bytea,
    '\x11223344'::bytea,
    'grp-v3-xor',
    NULL,
    true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    3
) IS NOT NULL AS created_grp_v3;

SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    '0123abc'::text
) = pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_xor_v3'),
    '0123abc'::text
) AS fell_back_to_newly_inserted_variant_3;

-- Also verify altering a context dictionary invalidates its cached EncodingContext
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    'Invalidate test'::text
) AS before_alter_bytes \gset

SELECT pg_alter_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    '\x11223344'::bytea,
    NULL,
    'v2-sym-updated',
    ARRAY['mode=xor-v2']
);

SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    'Invalidate test'::text
) <> :'before_alter_bytes'::bytea AS cache_invalidated;

-- 5. Context Group & Variant validation rules
-- 5a. Variant out of uint8 range (0..255) must be rejected
SELECT pg_create_encoding_context(
    'ctx_bad_variant_neg',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype, NULL, NULL, NULL, NULL, true, 0, -1
);

SELECT pg_create_encoding_context(
    'ctx_bad_variant_256',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype, NULL, NULL, NULL, NULL, true, 0, 256
);

-- 5b. Standalone context (variant = 0) cannot specify an external group_id
SELECT pg_create_encoding_context(
    'ctx_bad_standalone_grp',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype, NULL, NULL, NULL, NULL, true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    0
);

-- 5c. Referencing a standalone context (ectxvariant = 0) as group leader must be rejected
SELECT pg_create_encoding_context(
    'ctx_bad_ref_standalone',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype, NULL, NULL, NULL, NULL, true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_sym_text'),
    2
);

-- 5d. Duplicate (ectxgroup, ectxvariant) within the same group must be rejected
SELECT pg_create_encoding_context(
    'ctx_bad_dup_variant',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'text'::regtype, NULL, NULL, NULL, NULL, true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    3
);

-- 5e. Target type mismatch with group leader must be rejected
SELECT pg_create_encoding_context(
    'ctx_bad_grp_target_type',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'test_dict_type'),
    'int4'::regtype, NULL, NULL, NULL, NULL, true,
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
    4
);

-- 6. Column binding (pg_set_column_encoding_context) and dependency tracking
CREATE TABLE test_ectx_tbl (
    id int,
    payload text
);

-- Type mismatch: ctx_grp_digits has target type 'text', cannot bind to 'id' (int4)
SELECT pg_set_column_encoding_context(
    'test_ectx_tbl',
    'id',
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits')
);

-- Valid binding to 'payload' (text)
SELECT pg_set_column_encoding_context(
    'test_ectx_tbl',
    'payload',
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits')
);

SELECT a.attname, c.ectxname
FROM pg_attribute a
JOIN pg_encoding_context c ON c.oid = a.attencoding_context
WHERE a.attrelid = 'test_ectx_tbl'::regclass;

-- Dropping referenced group leader with RESTRICT (cascade = false) must fail
-- because both test_ectx_tbl.payload and group variants (v3, v5) depend on it
SELECT pg_drop_encoding_context(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ctx_grp_digits'),
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

