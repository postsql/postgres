--
-- pg_cascade: Multi-Stage Cascade & Fused Avro+Zstd Encoding Contexts
--

CREATE EXTENSION pg_avro;
CREATE EXTENSION pg_zstd;
CREATE EXTENSION pg_lz4;
CREATE EXTENSION pg_cascade;

-- 1. Verify default 'cascade' and 'avro_zstd' encoding types created by extension
SELECT enctypname, enctypkind, enctyptype, enctypencfunc, enctypdecfunc
FROM pg_encoding_type
WHERE enctypname IN ('cascade', 'avro_zstd')
ORDER BY enctypname;

-- 2. Stage-1 Avro context + pg_cascade_train_dictionary (for both Zstd and LZ4)
CREATE ENCODING CONTEXT cascade_avro_stage1_ctx
    TYPE avro
    FOR TYPE jsonb
    WITH (
        LABEL = 'cascade-avro-stage1',
        schema = '{
            "type": "record",
            "name": "UserEvent",
            "fields": [
                {"name": "user_id", "type": "long"},
                {"name": "username", "type": "string"},
                {"name": "active", "type": "boolean", "default": true},
                {"name": "score", "type": "double"},
                {"name": "role", "type": {"type": "enum", "name": "Role", "symbols": ["GUEST", "USER", "ADMIN"]}},
                {"name": "tags", "type": {"type": "array", "items": "string"}},
                {"name": "attrs", "type": {"type": "map", "values": "int"}},
                {"name": "note", "type": ["null", "string"], "default": null}
            ]
        }'
    );

CREATE TABLE avro_events_tbl (
    id int PRIMARY KEY,
    event jsonb
);

ALTER TABLE avro_events_tbl ALTER COLUMN event SET ENCODING CONTEXT cascade_avro_stage1_ctx;

INSERT INTO avro_events_tbl
SELECT i,
       jsonb_build_object(
           'user_id', 100000 + i,
           'username', 'service_account_' || (i % 10),
           'active', (i % 2 = 0),
           'score', 85.5 + (i % 15),
           'role', CASE WHEN i % 3 = 0 THEN 'ADMIN' ELSE 'USER' END,
           'tags', jsonb_build_array('production', 'kubernetes', 'us-east-1', 'tier-1'),
           'attrs', jsonb_build_object('cluster_id', 42, 'replica_set', i % 4),
           'note', 'automated telemetry event payload for composable avro+zstd pipeline'
       )
FROM generate_series(1, 50) g(i);

-- Negative tests for pg_cascade_train_dictionary
SELECT pg_cascade_train_dictionary(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
    ARRAY[]::jsonb[],
    4096,
    'zstd'
);
SELECT pg_cascade_train_dictionary(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
    (SELECT array_agg(event) FROM avro_events_tbl WHERE id <= 6),
    4096,
    'unknown_algo'
);

-- Train Stage-2 Zstd and LZ4 dictionaries directly from SQL jsonb samples via pg_cascade_train_dictionary!
DO $$
DECLARE
    v_zstd_dict bytea;
    v_lz4_dict bytea;
BEGIN
    SELECT pg_cascade_train_dictionary(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
        array_agg(event),
        16384,
        'zstd'
    )
    INTO v_zstd_dict
    FROM avro_events_tbl;

    IF v_zstd_dict IS NULL OR octet_length(v_zstd_dict) = 0 THEN
        RAISE EXCEPTION 'pg_cascade_train_dictionary (zstd) returned empty dictionary';
    END IF;

    SELECT pg_cascade_train_dictionary(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
        array_agg(event),
        16384,
        'lz4'
    )
    INTO v_lz4_dict
    FROM avro_events_tbl;

    IF v_lz4_dict IS NULL OR octet_length(v_lz4_dict) = 0 THEN
        RAISE EXCEPTION 'pg_cascade_train_dictionary (lz4) returned empty dictionary';
    END IF;

    -- Create Stage-2 standalone Zstd and LZ4 contexts for WITH (stages = ...)
    PERFORM pg_create_encoding_context(
        'cascade_zstd_stage2_ctx',
        (SELECT oid FROM pg_encoding_type WHERE enctypname = 'zstd'),
        0,
        'bytea'::regtype,
        v_zstd_dict,
        NULL,
        'cascade-zstd-stage2-dict',
        ARRAY['level=5']
    );

    PERFORM pg_create_encoding_context(
        'cascade_lz4_stage2_ctx',
        (SELECT oid FROM pg_encoding_type WHERE enctypname = 'lz4'),
        0,
        'bytea'::regtype,
        v_lz4_dict,
        NULL,
        'cascade-lz4-stage2-dict',
        ARRAY['acceleration=1']
    );

    -- Also create PARENT-chained Avro -> Zstd context
    PERFORM pg_create_encoding_context(
        'avro_plus_zstd_parent_ctx',
        (SELECT oid FROM pg_encoding_type WHERE enctypname = 'zstd'),
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
        0,
        v_zstd_dict,
        NULL,
        'composable-avro-plus-zstd-dict',
        ARRAY['level=5']
    );
END;
$$;

-- 3. Multi-Stage Cascade Contexts using TYPE cascade WITH (stages = '...')
CREATE ENCODING CONTEXT avro_zstd_cascade_ctx
    TYPE cascade
    FOR TYPE jsonb
    WITH (
        LABEL = 'avro-then-zstd-cascade',
        stages = 'cascade_avro_stage1_ctx, cascade_zstd_stage2_ctx'
    );

CREATE ENCODING CONTEXT avro_lz4_cascade_ctx
    TYPE cascade
    FOR TYPE jsonb
    WITH (
        LABEL = 'avro-then-lz4-cascade',
        stages = 'cascade_avro_stage1_ctx, cascade_lz4_stage2_ctx'
    );

-- Negative tests for cascade context options (missing stages, empty stage, circular reference)
CREATE ENCODING CONTEXT bad_cascade_no_stages_ctx TYPE cascade FOR TYPE jsonb;
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'bad_cascade_no_stages_ctx'),
    '{}'::jsonb
);
DROP ENCODING CONTEXT bad_cascade_no_stages_ctx;

CREATE ENCODING CONTEXT bad_cascade_empty_stage_ctx TYPE cascade FOR TYPE jsonb WITH (
    stages = 'cascade_avro_stage1_ctx, '
);
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'bad_cascade_empty_stage_ctx'),
    '{}'::jsonb
);
DROP ENCODING CONTEXT bad_cascade_empty_stage_ctx;

CREATE ENCODING CONTEXT bad_cascade_self_ctx TYPE cascade FOR TYPE jsonb WITH (
    stages = 'bad_cascade_self_ctx'
);
SELECT pg_encode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'bad_cascade_self_ctx'),
    '{}'::jsonb
);
DROP ENCODING CONTEXT bad_cascade_self_ctx;

-- 4. Single-Stage Fused Handler (TYPE = 'avro_zstd')
SELECT pg_create_encoding_context(
    'avro_zstd_single_ctx',
    (SELECT oid FROM pg_encoding_type WHERE enctypname = 'avro_zstd'),
    0,
    'jsonb'::regtype,
    (SELECT ectxenctab FROM pg_encoding_context WHERE ectxname = 'cascade_zstd_stage2_ctx'),
    NULL,
    'single-stage-avro-zstd',
    ARRAY[
        'level=5',
        'schema={
            "type": "record",
            "name": "UserEvent",
            "fields": [
                {"name": "user_id", "type": "long"},
                {"name": "username", "type": "string"},
                {"name": "active", "type": "boolean", "default": true},
                {"name": "score", "type": "double"},
                {"name": "role", "type": {"type": "enum", "name": "Role", "symbols": ["GUEST", "USER", "ADMIN"]}},
                {"name": "tags", "type": {"type": "array", "items": "string"}},
                {"name": "attrs", "type": {"type": "map", "values": "int"}},
                {"name": "note", "type": ["null", "string"], "default": null}
            ]
        }'
    ]
) IS NOT NULL AS created_single_stage_ctx;

-- 5. Verify byte-for-byte equivalence and round-trip across:
--    (a) PARENT-chained (avro_plus_zstd_parent_ctx)
--    (b) TYPE cascade WITH (stages = ...) (avro_zstd_cascade_ctx)
--    (c) Fused single-stage avro_zstd (avro_zstd_single_ctx)
--    (d) TYPE cascade WITH (stages = 'avro, lz4') (avro_lz4_cascade_ctx)
WITH sample_event AS (
    SELECT event || '{}'::jsonb AS doc FROM avro_events_tbl WHERE id = 1
),
encoded_variants AS (
    SELECT doc,
           pg_column_size(doc) AS jsonb_size,
           pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'), doc) AS avro_only_bytes,
           pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_plus_zstd_parent_ctx'), doc) AS parent_zstd_bytes,
           pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_cascade_ctx'), doc) AS cascade_zstd_bytes,
           pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_single_ctx'), doc) AS single_zstd_bytes,
           pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_lz4_cascade_ctx'), doc) AS cascade_lz4_bytes
    FROM sample_event
)
SELECT octet_length(avro_only_bytes) < jsonb_size AS avro_smaller_than_jsonb,
       octet_length(cascade_zstd_bytes) < octet_length(avro_only_bytes) AS cascade_zstd_smaller_than_avro,
       octet_length(cascade_lz4_bytes) < octet_length(avro_only_bytes) AS cascade_lz4_smaller_than_avro,
       cascade_zstd_bytes = parent_zstd_bytes AS cascade_matches_parent_bytes,
       single_zstd_bytes = cascade_zstd_bytes AS single_matches_cascade_bytes,
       pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_cascade_ctx'), cascade_zstd_bytes, NULL::jsonb) = doc AS cascade_zstd_roundtrip_ok,
       pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_single_ctx'), single_zstd_bytes, NULL::jsonb) = doc AS single_zstd_roundtrip_ok,
       pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_lz4_cascade_ctx'), cascade_lz4_bytes, NULL::jsonb) = doc AS cascade_lz4_roundtrip_ok
FROM encoded_variants;

-- 6. Online Context Switching across Standalone Avro -> Cascade (Avro+Zstd) -> Fused avro_zstd -> Cascade (Avro+LZ4)
ALTER TABLE avro_events_tbl ALTER COLUMN event SET ENCODING CONTEXT avro_zstd_cascade_ctx;

INSERT INTO avro_events_tbl
SELECT i,
       jsonb_build_object(
           'user_id', 100000 + i,
           'username', 'service_account_' || (i % 10),
           'active', true,
           'score', 91.0,
           'role', 'ADMIN',
           'tags', jsonb_build_array('production', 'kubernetes', 'us-east-1', 'tier-1'),
           'attrs', jsonb_build_object('cluster_id', 42, 'replica_set', i % 4),
           'note', repeat('large multi-chunk avro+zstd event note payload ', 120)
       )
FROM generate_series(51, 53) g(i);

ALTER TABLE avro_events_tbl ALTER COLUMN event SET ENCODING CONTEXT avro_zstd_single_ctx;

INSERT INTO avro_events_tbl VALUES (
    54,
    '{
        "user_id": 100054,
        "username": "single_stage_user",
        "active": true,
        "score": 99.9,
        "role": "ADMIN",
        "tags": ["production", "kubernetes", "us-east-1", "tier-1"],
        "attrs": {"cluster_id": 42, "replica_set": 0},
        "note": "encoded via single-stage avro_zstd handler"
    }'::jsonb
);

ALTER TABLE avro_events_tbl ALTER COLUMN event SET ENCODING CONTEXT avro_lz4_cascade_ctx;

INSERT INTO avro_events_tbl VALUES (
    55,
    '{
        "user_id": 100055,
        "username": "cascade_lz4_user",
        "active": false,
        "score": 88.0,
        "role": "USER",
        "tags": ["production", "kubernetes", "us-east-1", "tier-1"],
        "attrs": {"cluster_id": 42, "replica_set": 1},
        "note": "encoded via cascade avro+lz4 handler"
    }'::jsonb
);

SELECT id,
       event->>'username' AS username,
       (event->>'user_id')::bigint AS user_id,
       octet_length(pg_column_raw_encoding(event)) < octet_length(event::text) AS compressed_on_disk,
       length(event->>'note') AS note_len
FROM avro_events_tbl
WHERE id IN (1, 51, 54, 55)
ORDER BY id;

-- 7. Fused avro_zstd DDL schema evolution (writer_schema + reader_schema) & dictionary precedence
CREATE ENCODING CONTEXT avro_zstd_ddl_evolved_ctx TYPE avro_zstd FOR TYPE jsonb WITH (
    writer_schema = '{"type": "record", "name": "Ev", "fields": [{"name": "id", "type": "int"}, {"name": "legacy", "type": "string"}]}',
    reader_schema = '{"type": "record", "name": "Ev", "fields": [{"name": "id", "type": "long"}, {"name": "status", "type": "string", "default": "migrated"}]}',
    level = 3
);
CREATE ENCODING CONTEXT avro_zstd_schema_dict_ddl_ctx TYPE avro_zstd FOR TYPE jsonb WITH (
    schema = '"string"',
    dictionary = '\x73616d706c655f646963745f7072656669785f30313233343536373839'
);

SELECT pg_decode_datum(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_ddl_evolved_ctx'),
           pg_encode_datum(
               (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_ddl_evolved_ctx'),
               '{"id": 99, "legacy": "old"}'::jsonb
           ),
           NULL::jsonb
       ) AS avro_zstd_ddl_evolved,
       (SELECT ectxenctab FROM pg_encoding_context WHERE ectxname = 'avro_zstd_schema_dict_ddl_ctx')
           = '\x73616d706c655f646963745f7072656669785f30313233343536373839'::bytea
           AS dict_takes_precedence_in_enctab,
       pg_decode_datum(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_schema_dict_ddl_ctx'),
           pg_encode_datum(
               (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_zstd_schema_dict_ddl_ctx'),
               '"sample_dict_prefix_0123456789_payload"'::jsonb
           ),
           NULL::jsonb
       ) = '"sample_dict_prefix_0123456789_payload"'::jsonb AS schema_dict_roundtrip_ok;

DROP ENCODING CONTEXT avro_zstd_schema_dict_ddl_ctx;
DROP ENCODING CONTEXT avro_zstd_ddl_evolved_ctx;

-- 7b. Indirect mutual cycle detection, Stage-0 FOR TYPE inheritance, and small-corpus training
CREATE ENCODING CONTEXT bad_cascade_cycle_a TYPE cascade FOR TYPE jsonb;
CREATE ENCODING CONTEXT bad_cascade_cycle_b TYPE cascade FOR TYPE jsonb WITH (
    stages = 'bad_cascade_cycle_a'
);
DROP ENCODING CONTEXT bad_cascade_cycle_a;
CREATE ENCODING CONTEXT bad_cascade_cycle_a TYPE cascade FOR TYPE jsonb WITH (
    stages = 'bad_cascade_cycle_b'
);
DO $$
BEGIN
    PERFORM pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'bad_cascade_cycle_a'),
        '{}'::jsonb
    );
    RAISE EXCEPTION 'expected cycle error was not raised';
EXCEPTION WHEN invalid_object_definition THEN
    RAISE NOTICE 'caught expected indirect cycle error: %', regexp_replace(SQLERRM, '[0-9]+', 'OID', 'g');
END;
$$;
DROP ENCODING CONTEXT bad_cascade_cycle_a;
DROP ENCODING CONTEXT bad_cascade_cycle_b;

-- Cascade context without outer FOR TYPE (inherits target type and whole-datum from Stage 0)
CREATE ENCODING CONTEXT avro_zstd_implicit_type_cascade_ctx TYPE cascade WITH (
    stages = 'cascade_avro_stage1_ctx, cascade_zstd_stage2_ctx'
);
CREATE TEMP TABLE implicit_type_cascade_tbl (
    id int PRIMARY KEY,
    doc jsonb
);
ALTER TABLE implicit_type_cascade_tbl ALTER COLUMN doc SET ENCODING CONTEXT avro_zstd_implicit_type_cascade_ctx;
INSERT INTO implicit_type_cascade_tbl VALUES (
    1,
    jsonb_build_object(
        'user_id', 777,
        'username', 'implicit_stage0_type',
        'active', true,
        'score', 95.5,
        'role', 'ADMIN',
        'tags', jsonb_build_array('production', 'tier-1'),
        'attrs', jsonb_build_object('cluster_id', 42),
        'note', repeat('multi-chunk stage0 type inheritance test ', 100)
    )
);
SELECT id,
       doc->>'username' AS username,
       length(doc->>'note') AS note_len,
       octet_length(pg_column_raw_encoding(doc)) < octet_length(doc::text) AS compressed_ok,
       octet_length(pg_cascade_train_dictionary(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'cascade_avro_stage1_ctx'),
           ARRAY[doc, doc],
           2048,
           'zstd'
       )) > 0 AS small_sample_zstd_dict_ok
FROM implicit_type_cascade_tbl;
DROP TABLE implicit_type_cascade_tbl;
DROP ENCODING CONTEXT avro_zstd_implicit_type_cascade_ctx;

-- 8. Unprivileged role security check
CREATE ROLE regress_cascade_unpriv;
GRANT SELECT ON avro_events_tbl TO regress_cascade_unpriv;
SET ROLE regress_cascade_unpriv;

SELECT id, event->>'username' FROM avro_events_tbl WHERE id IN (1, 51, 54, 55) ORDER BY id;
CREATE ENCODING CONTEXT unpriv_cascade_ctx TYPE cascade WITH (stages = 'cascade_avro_stage1_ctx');

RESET ROLE;
REVOKE ALL ON avro_events_tbl FROM regress_cascade_unpriv;
DROP ROLE regress_cascade_unpriv;

-- Clean up
DROP TABLE avro_events_tbl;
DROP ENCODING CONTEXT avro_zstd_single_ctx;
DROP ENCODING CONTEXT avro_lz4_cascade_ctx;
DROP ENCODING CONTEXT avro_zstd_cascade_ctx;
DROP ENCODING CONTEXT avro_plus_zstd_parent_ctx;
DROP ENCODING CONTEXT cascade_lz4_stage2_ctx;
DROP ENCODING CONTEXT cascade_zstd_stage2_ctx;
DROP ENCODING CONTEXT cascade_avro_stage1_ctx;
DROP EXTENSION pg_cascade;
DROP EXTENSION pg_lz4;
DROP EXTENSION pg_zstd;
DROP EXTENSION pg_avro;
