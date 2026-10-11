--
-- pg_avro: Apache Avro Binary Serialization & Schema Evolution Encoding Contexts
--

CREATE EXTENSION pg_avro;

-- 1. Verify default 'avro' encoding type created by extension
SELECT enctypname, enctypkind, enctyptype, enctypencfunc, enctypdecfunc
FROM pg_encoding_type
WHERE enctypname = 'avro';

-- 2. Schema validation (pg_avro_validate_schema)
SELECT pg_avro_validate_schema('"string"');
SELECT pg_avro_validate_schema('{
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
}');

-- Invalid Avro schemas must fail validation
SELECT pg_avro_validate_schema('{"type": "unknown_avro_type"}');
SELECT pg_avro_validate_schema('{"type": "record", "name": "BadRec"}');
SELECT pg_avro_validate_schema('[["null", "int"]]');
SELECT pg_avro_validate_schema('["int", "int"]');

-- 3. Auto-resolved ENCFUNC / DECFUNC when TYPE = 'avro'
CREATE ENCODING TYPE avro_enc_type (
    KIND = 's',
    TYPE = 'avro'
);

SELECT enctypname, enctypkind, enctyptype, enctypencfunc, enctypdecfunc
FROM pg_encoding_type
WHERE enctypname = 'avro_enc_type';

-- 4. Create Avro encoding context for jsonb
CREATE ENCODING CONTEXT avro_user_v1_ctx
    TYPE avro
    FOR TYPE jsonb
    WITH (
        LABEL = 'avro-user-event-v1',
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

-- Verify compact binary Avro encoding vs JSONB size and lossless round-trip
WITH sample AS (
    SELECT '{
        "user_id": 9876543210,
        "username": "alice_postgres",
        "active": true,
        "score": 99.5,
        "role": "ADMIN",
        "tags": ["storage", "toast", "avro"],
        "attrs": {"retries": 3, "shard": 12},
        "note": "verified binary encoding"
    }'::jsonb AS doc
),
encoded AS (
    SELECT doc,
           pg_encode_datum(
               (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_v1_ctx'),
               doc
           ) AS avro_bytes
    FROM sample
)
SELECT octet_length(avro_bytes) < pg_column_size(doc) AS avro_smaller_than_jsonb,
       octet_length(avro_bytes) AS avro_byte_len,
       pg_decode_datum(
           (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_v1_ctx'),
           avro_bytes,
           NULL::jsonb
       ) = doc AS avro_jsonb_roundtrip_ok
FROM encoded;

-- Verify nullable union branch ("note": null) and default field filling when omitted on input
SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_v1_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_v1_ctx'),
        '{"user_id": 42, "username": "bob", "score": 12.25, "role": "USER", "tags": [], "attrs": {}}'::jsonb
    ),
    NULL::jsonb
) AS avro_defaults_and_null_union;

-- 5. Avro Schema Evolution (Writer Schema v1 -> Reader Schema v2)
CREATE ENCODING CONTEXT avro_user_evolved_ctx
    TYPE avro
    FOR TYPE jsonb
    WITH (
        LABEL = 'avro-user-v1-to-v2',
        writer_schema = '{
            "type": "record",
            "name": "UserEvent",
            "fields": [
                {"name": "user_id", "type": "int"},
                {"name": "username", "type": "string"},
                {"name": "legacy_flag", "type": "boolean"},
                {"name": "role", "type": {"type": "enum", "name": "Role", "symbols": ["GUEST", "USER", "ADMIN"]}}
            ]
        }',
        reader_schema = '{
            "type": "record",
            "name": "UserEvent",
            "fields": [
                {"name": "username", "type": "string"},
                {"name": "user_id", "type": "long"},
                {"name": "role", "type": {"type": "enum", "name": "Role", "symbols": ["GUEST", "USER", "ADMIN", "SUPERADMIN"]}},
                {"name": "region", "type": "string", "default": "global"},
                {"name": "schema_version", "type": "int", "default": 2}
            ]
        }'
    );

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_evolved_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_user_evolved_ctx'),
        '{"user_id": 1001, "username": "carol", "legacy_flag": true, "role": "ADMIN"}'::jsonb
    ),
    NULL::jsonb
) AS evolved_record_jsonb;

-- 6. SQL Composite Type with Apache Avro Context & Composite Evolution
CREATE TYPE avro_sensor_reading AS (
    sensor_id int4,
    device_name text,
    temperature float8,
    is_valid bool
);

CREATE ENCODING CONTEXT avro_composite_ctx
    TYPE avro
    FOR TYPE avro_sensor_reading
    WITH (
        schema = '{
            "type": "record",
            "name": "SensorReading",
            "fields": [
                {"name": "sensor_id", "type": "int"},
                {"name": "device_name", "type": "string"},
                {"name": "temperature", "type": "double"},
                {"name": "is_valid", "type": "boolean"}
            ]
        }'
    );

SELECT (pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_composite_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_composite_ctx'),
        ROW(77, 'thermostat-north', 21.75, true)::avro_sensor_reading
    ),
    NULL::avro_sensor_reading
)).*;

CREATE TYPE avro_sensor_v2 AS (
    sensor_id int4,
    temperature float8,
    unit text
);

CREATE ENCODING CONTEXT avro_composite_evolved_ctx TYPE avro FOR TYPE avro_sensor_v2 WITH (
    writer_schema = '{
        "type": "record",
        "name": "SensorReading",
        "fields": [
            {"name": "sensor_id", "type": "int"},
            {"name": "location", "type": "string"},
            {"name": "temperature", "type": "double"},
            {"name": "Alert", "type": "boolean"}
        ]
    }',
    reader_schema = '{
        "type": "record",
        "name": "SensorReading",
        "fields": [
            {"name": "sensor_id", "type": "int"},
            {"name": "temperature", "type": "double"},
            {"name": "unit", "type": "string", "default": "celsius"}
        ]
    }'
);

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_composite_evolved_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_composite_ctx'),
        ROW(7, 'boiler-room', 102.5, true)::avro_sensor_reading
    ),
    NULL::avro_sensor_v2
) AS evolved_composite_row;

DROP ENCODING CONTEXT avro_composite_evolved_ctx;
DROP TYPE avro_sensor_v2;

-- 7. Avro Root Union & Multi-Branch Union Disambiguation + Fractional Int Rejection
CREATE ENCODING CONTEXT avro_root_union_ctx TYPE avro FOR TYPE jsonb WITH (
    schema = '["null", "int", "long", "double", {"type": "enum", "name": "Color", "symbols": ["RED", "GREEN"]}, {"type": "fixed", "name": "F4", "size": 4}, "string", {"type": "record", "name": "RecA", "fields": [{"name": "a", "type": "int"}]}, {"type": "record", "name": "RecB", "fields": [{"name": "b", "type": "string"}]}]'
);

SELECT pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_root_union_ctx'),
                       pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_root_union_ctx'), v),
                       NULL::jsonb) = v AS roundtrip_ok,
       pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_root_union_ctx'),
                       pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_root_union_ctx'), v),
                       NULL::jsonb) AS decoded_val
FROM (VALUES
    ('null'::jsonb),
    ('42'::jsonb),
    ('9876543210'::jsonb),
    ('3.14159'::jsonb),
    ('"RED"'::jsonb),
    ('"ABCD"'::jsonb),
    ('"general_string"'::jsonb),
    ('{"a": 7}'::jsonb),
    ('{"b": "hello"}'::jsonb)
) AS t(v);

CREATE ENCODING CONTEXT avro_strict_int_ctx TYPE avro FOR TYPE jsonb WITH (
    schema = '"int"'
);
SELECT pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_strict_int_ctx'), '3.75'::jsonb);
SELECT pg_decode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_strict_int_ctx'),
                       pg_encode_datum((SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_strict_int_ctx'), '123'::jsonb),
                       NULL::jsonb) AS int_after_error_ok;

DROP ENCODING CONTEXT avro_strict_int_ctx;
DROP ENCODING CONTEXT avro_root_union_ctx;

-- 8. Avro Binary Hex (\x...) Round-Trip for "bytes" and "fixed" in JSONB
CREATE ENCODING CONTEXT avro_bin_hex_ctx TYPE avro FOR TYPE jsonb WITH (
    schema = '{"type": "record", "name": "BinRec", "fields": [{"name": "raw_bytes", "type": "bytes"}, {"name": "magic", "type": {"type": "fixed", "name": "Magic4", "size": 4}}]}'
);

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_bin_hex_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_bin_hex_ctx'),
        '{"raw_bytes": "\\xdeadbeef00ff", "magic": "\\x00010203"}'::jsonb
    ),
    NULL::jsonb
) AS bin_hex_roundtrip;

DROP ENCODING CONTEXT avro_bin_hex_ctx;

-- 9. Recursive Avro Schema with Namespace and Schema Evolution
CREATE ENCODING CONTEXT avro_list_v1_ctx TYPE avro FOR TYPE jsonb WITH (
    schema = '{"type": "record", "name": "Node", "namespace": "org.example", "fields": [{"name": "val", "type": "int"}, {"name": "next", "type": ["null", "org.example.Node"], "default": null}]}'
);
CREATE ENCODING CONTEXT avro_list_v2_ctx TYPE avro FOR TYPE jsonb WITH (
    writer_schema = '{"type": "record", "name": "Node", "namespace": "org.example", "fields": [{"name": "val", "type": "int"}, {"name": "next", "type": ["null", "org.example.Node"], "default": null}]}',
    reader_schema = '{"type": "record", "name": "Node", "namespace": "org.example", "fields": [{"name": "val", "type": "long"}, {"name": "tag", "type": "string", "default": "v2"}, {"name": "next", "type": ["null", "org.example.Node"], "default": null}]}'
);

SELECT pg_decode_datum(
    (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_list_v2_ctx'),
    pg_encode_datum(
        (SELECT oid FROM pg_encoding_context WHERE ectxname = 'avro_list_v1_ctx'),
        '{"val": 1, "next": {"val": 2, "next": {"val": 3, "next": null}}}'::jsonb
    ),
    NULL::jsonb
) AS recursive_evolved_list;

DROP ENCODING CONTEXT avro_list_v2_ctx;
DROP ENCODING CONTEXT avro_list_v1_ctx;

-- 10. Direct TOAST table column with Avro context + unprivileged role check
CREATE TABLE avro_toast_tbl (
    id int PRIMARY KEY,
    event jsonb
);
ALTER TABLE avro_toast_tbl ALTER COLUMN event SET ENCODING CONTEXT avro_user_v1_ctx;

INSERT INTO avro_toast_tbl VALUES (
    1,
    jsonb_build_object(
        'user_id', 9001,
        'username', 'toast_avro_user',
        'active', true,
        'score', 100.0,
        'role', 'ADMIN',
        'tags', jsonb_build_array('tier1', 'direct_toast'),
        'attrs', jsonb_build_object('k', 1),
        'note', repeat('large_avro_note_', 200)
    )
);

SELECT id,
       event->>'username' AS username,
       length(event->>'note') AS note_len,
       octet_length(pg_column_raw_encoding(event)) < pg_column_size(event || '{}'::jsonb) AS avro_smaller
FROM avro_toast_tbl;

CREATE ROLE regress_avro_unpriv;
GRANT SELECT ON avro_toast_tbl TO regress_avro_unpriv;
SET ROLE regress_avro_unpriv;
SELECT id, event->>'username' FROM avro_toast_tbl;
CREATE ENCODING CONTEXT unpriv_avro_ctx TYPE avro;
RESET ROLE;
REVOKE ALL ON avro_toast_tbl FROM regress_avro_unpriv;
DROP ROLE regress_avro_unpriv;

-- Clean up
DROP TABLE avro_toast_tbl;
DROP ENCODING CONTEXT avro_composite_ctx;
DROP TYPE avro_sensor_reading;
DROP ENCODING CONTEXT avro_user_evolved_ctx;
DROP ENCODING CONTEXT avro_user_v1_ctx;
DROP ENCODING TYPE avro_enc_type;
DROP EXTENSION pg_avro;
