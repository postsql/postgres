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

-- =========================================================================
-- 9. SQL DDL Grammar & Utility Commands (Patch 2)
-- =========================================================================

-- 9a. CREATE ENCODING TYPE & IF NOT EXISTS
CREATE ENCODING TYPE ddl_dict_type (
    KIND = 's',
    TYPE = 'xor_lut',
    ENCFUNC = pg_bytea_dict_encode,
    DECFUNC = pg_bytea_dict_decode
);

CREATE ENCODING TYPE IF NOT EXISTS ddl_dict_type (
    KIND = 's',
    TYPE = 'xor_lut',
    ENCFUNC = pg_bytea_dict_encode,
    DECFUNC = pg_bytea_dict_decode
);

CREATE ENCODING TYPE ddl_pglz_type (
    KIND = 'c',
    ALGORITHM = 'pglz',
    ENCFUNC = pg_pglz_context_encode,
    DECFUNC = pg_pglz_context_decode
);

-- Reject unrecognized parameter in CREATE ENCODING TYPE
CREATE ENCODING TYPE ddl_bad_param_type (
    KIND = 'c',
    TYPE = 'pglz',
    BOGUS = 'nope'
);

-- 9b. CREATE ENCODING CONTEXT (clause syntax & definition syntax)
CREATE ENCODING CONTEXT ddl_root_ctx
    TYPE ddl_dict_type
    FOR TYPE text
    WITH (
        ENCTAB = '\xaa55aa55',
        LABEL = 'ddl-v1-root',
        custom_opt = 'enabled'
    );

CREATE ENCODING CONTEXT IF NOT EXISTS ddl_root_ctx
    TYPE ddl_dict_type
    FOR TYPE text;

-- Chained child context via PARENT clause
CREATE ENCODING CONTEXT ddl_child_ctx
    TYPE ddl_pglz_type
    PARENT ddl_root_ctx
    WITH (
        LABEL = 'ddl-v1-child-pglz',
        level = 6
    );

-- Parenthesized definition syntax for CREATE ENCODING CONTEXT
CREATE ENCODING CONTEXT ddl_parens_ctx (
    TYPE = ddl_dict_type,
    FOR_TYPE = bytea,
    ENCTAB = '\x0f0f',
    LABEL = 'ddl-parens'
);

SELECT c.ectxname,
       t.enctypname,
       p.ectxname AS parent_ctx,
       c.ectxtargettypid::regtype AS target_type,
       c.ectxlabel,
       encode(c.ectxenctab, 'hex') AS enctab_hex,
       c.ectxoptions
FROM pg_encoding_context c
JOIN pg_encoding_type t ON t.oid = c.ectxtypid
LEFT JOIN pg_encoding_context p ON p.oid = c.ectxparentid
ORDER BY c.ectxname;

-- Verify composable round-trip on DDL-created context pipeline
SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ddl_child_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'ddl_child_ctx'),
        'DDL Composable Pipeline Test!'::text
    ),
    NULL::text
) AS ddl_pipeline_roundtrip;

-- 9c. ALTER ENCODING CONTEXT (SET / WITH / bare parens, and clearing via NULL/NONE)
ALTER ENCODING CONTEXT ddl_root_ctx SET (
    ENCTAB = '\x12345678',
    DECTAB = '\x12345678',
    LABEL = 'ddl-v2-root',
    custom_opt = 'updated'
);

SELECT ectxlabel,
       encode(ectxenctab, 'hex') AS enctab_hex,
       encode(ectxdectab, 'hex') AS dectab_hex,
       ectxoptions
FROM pg_encoding_context
WHERE ectxname = 'ddl_root_ctx';

-- Clear dectab and label using NULL / NONE
ALTER ENCODING CONTEXT ddl_root_ctx WITH (
    DECTAB = NULL,
    LABEL = NONE
);

SELECT ectxlabel IS NULL AS label_cleared,
       ectxdectab IS NULL AS dectab_cleared,
       encode(ectxenctab, 'hex') AS enctab_preserved
FROM pg_encoding_context
WHERE ectxname = 'ddl_root_ctx';

-- 9d. ALTER TABLE ... ALTER [COLUMN] ... SET / DROP ENCODING CONTEXT
CREATE TABLE ddl_ectx_tbl (
    id int,
    doc text
);

-- Type mismatch must fail
ALTER TABLE ddl_ectx_tbl ALTER COLUMN id SET ENCODING CONTEXT ddl_child_ctx;

-- Valid column binding via DDL
ALTER TABLE ddl_ectx_tbl ALTER COLUMN doc SET ENCODING CONTEXT ddl_child_ctx;

SELECT a.attname, c.ectxname
FROM pg_attribute a
JOIN pg_encoding_context c ON c.oid = a.attencoding_context
WHERE a.attrelid = 'ddl_ectx_tbl'::regclass;

-- Reset via DROP ENCODING CONTEXT and SET ENCODING CONTEXT DEFAULT
ALTER TABLE ddl_ectx_tbl ALTER COLUMN doc DROP ENCODING CONTEXT;

SELECT attencoding_context FROM pg_attribute
WHERE attrelid = 'ddl_ectx_tbl'::regclass AND attname = 'doc';

ALTER TABLE ddl_ectx_tbl ALTER COLUMN doc SET ENCODING CONTEXT ddl_root_ctx;
ALTER TABLE ddl_ectx_tbl ALTER COLUMN doc SET ENCODING CONTEXT DEFAULT;

SELECT attencoding_context FROM pg_attribute
WHERE attrelid = 'ddl_ectx_tbl'::regclass AND attname = 'doc';

DROP TABLE ddl_ectx_tbl;

-- 9e. COMMENT ON, RENAME TO, SET SCHEMA, OWNER TO, pg_get_object_address
COMMENT ON ENCODING TYPE ddl_dict_type IS 'Dictionary encoding type via DDL';
COMMENT ON ENCODING CONTEXT ddl_root_ctx IS 'Root dictionary encoding context';

SELECT obj_description(oid, 'pg_encoding_type') FROM pg_encoding_type WHERE enctypname = 'ddl_dict_type';
SELECT obj_description(oid, 'pg_encoding_context') FROM pg_encoding_context WHERE ectxname = 'ddl_root_ctx';

SELECT type, schema, name, identity FROM pg_identify_object(
    (SELECT classid FROM pg_get_object_address('encoding type', ARRAY['ddl_dict_type'], '{}')),
    (SELECT objid FROM pg_get_object_address('encoding type', ARRAY['ddl_dict_type'], '{}')),
    0
);

SELECT type, schema, name, identity FROM pg_identify_object(
    (SELECT classid FROM pg_get_object_address('encoding context', ARRAY['ddl_root_ctx'], '{}')),
    (SELECT objid FROM pg_get_object_address('encoding context', ARRAY['ddl_root_ctx'], '{}')),
    0
);

CREATE SCHEMA ddl_ectx_schema;
ALTER ENCODING TYPE ddl_dict_type RENAME TO ddl_dict_type_renamed;
ALTER ENCODING TYPE ddl_dict_type_renamed SET SCHEMA ddl_ectx_schema;

ALTER ENCODING CONTEXT ddl_parens_ctx RENAME TO ddl_parens_ctx_renamed;
ALTER ENCODING CONTEXT ddl_parens_ctx_renamed SET SCHEMA ddl_ectx_schema;

SELECT n.nspname, t.enctypname
FROM pg_encoding_type t
JOIN pg_namespace n ON n.oid = t.enctypnamespace
WHERE t.enctypname = 'ddl_dict_type_renamed';

SELECT n.nspname, c.ectxname
FROM pg_encoding_context c
JOIN pg_namespace n ON n.oid = c.ectxnamespace
WHERE c.ectxname = 'ddl_parens_ctx_renamed';

-- 9f. Privilege checks on SQL DDL for unprivileged role
CREATE ROLE regress_ddl_encoding_user;
SET ROLE regress_ddl_encoding_user;

CREATE ENCODING TYPE unpriv_ddl_type (
    KIND = 'c',
    TYPE = 'pglz',
    ENCFUNC = pg_pglz_context_encode,
    DECFUNC = pg_pglz_context_decode
);

CREATE ENCODING CONTEXT unpriv_ddl_ctx
    TYPE ddl_pglz_type
    FOR TYPE text;

ALTER ENCODING CONTEXT ddl_root_ctx SET (LABEL = 'hacked');

RESET ROLE;

-- Superuser can transfer ownership
ALTER ENCODING CONTEXT ddl_root_ctx OWNER TO regress_ddl_encoding_user;
ALTER ENCODING TYPE ddl_pglz_type OWNER TO regress_ddl_encoding_user;

-- Still must be superuser to execute ALTER ENCODING CONTEXT even if owner
SET ROLE regress_ddl_encoding_user;
ALTER ENCODING CONTEXT ddl_root_ctx SET (LABEL = 'still-requires-superuser');
RESET ROLE;

-- Reassign back and drop role
REASSIGN OWNED BY regress_ddl_encoding_user TO CURRENT_USER;
DROP ROLE regress_ddl_encoding_user;

-- 9g. DROP ENCODING CONTEXT & DROP ENCODING TYPE (IF EXISTS, RESTRICT, CASCADE)
DROP ENCODING CONTEXT IF EXISTS non_existent_ctx;
DROP ENCODING TYPE IF EXISTS non_existent_type;

-- RESTRICT must fail while ddl_child_ctx depends on ddl_root_ctx
DROP ENCODING CONTEXT ddl_root_ctx RESTRICT;

-- Drop child first, then root
DROP ENCODING CONTEXT ddl_child_ctx;
DROP ENCODING CONTEXT ddl_root_ctx;

-- Drop remaining types and schema with CASCADE
DROP ENCODING TYPE ddl_pglz_type;
DROP SCHEMA ddl_ectx_schema CASCADE;

SELECT count(*) FROM pg_encoding_type;
SELECT count(*) FROM pg_encoding_context;
