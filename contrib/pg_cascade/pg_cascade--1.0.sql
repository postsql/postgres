/* contrib/pg_cascade/pg_cascade--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_cascade" to load this file. \quit

CREATE FUNCTION pg_cascade_context_encode(internal, anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cascade_context_encode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_cascade_context_decode(internal, bytea)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'pg_cascade_context_decode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_avro_zstd_context_encode(internal, anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_avro_zstd_context_encode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_avro_zstd_context_decode(internal, bytea)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'pg_avro_zstd_context_decode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_cascade_train_dictionary(
    stage_context_id oid,
    samples anyarray,
    max_dict_size int4 DEFAULT 65536,
    algorithm text DEFAULT 'zstd'
)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_cascade_train_dictionary'
LANGUAGE C VOLATILE STRICT PARALLEL SAFE;

CREATE ENCODING TYPE IF NOT EXISTS "cascade" (
    KIND = 'c',
    TYPE = 'cascade',
    ENCFUNC = pg_cascade_context_encode,
    DECFUNC = pg_cascade_context_decode
);

CREATE ENCODING TYPE IF NOT EXISTS avro_zstd (
    KIND = 's',
    TYPE = 'avro_zstd',
    ENCFUNC = pg_avro_zstd_context_encode,
    DECFUNC = pg_avro_zstd_context_decode
);

REVOKE EXECUTE ON FUNCTION pg_cascade_context_encode(internal, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_cascade_context_decode(internal, bytea) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_avro_zstd_context_encode(internal, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_avro_zstd_context_decode(internal, bytea) FROM PUBLIC;

