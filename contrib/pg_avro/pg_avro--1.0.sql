/* contrib/pg_avro/pg_avro--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_avro" to load this file. \quit

CREATE FUNCTION pg_avro_context_encode(internal, anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_avro_context_encode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_avro_context_decode(internal, bytea)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'pg_avro_context_decode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_avro_validate_schema(schema_json text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'pg_avro_validate_schema'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE ENCODING TYPE IF NOT EXISTS avro (
    KIND = 's',
    TYPE = 'avro',
    ENCFUNC = pg_avro_context_encode,
    DECFUNC = pg_avro_context_decode
);

REVOKE EXECUTE ON FUNCTION pg_avro_context_encode(internal, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_avro_context_decode(internal, bytea) FROM PUBLIC;
