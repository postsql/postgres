/* contrib/pg_zstd/pg_zstd--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_zstd" to load this file. \quit

CREATE FUNCTION pg_zstd_context_encode(internal, anyelement)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_zstd_context_encode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_zstd_context_decode(internal, bytea)
RETURNS anyelement
AS 'MODULE_PATHNAME', 'pg_zstd_context_decode'
LANGUAGE C IMMUTABLE STRICT PARALLEL SAFE;

CREATE FUNCTION pg_zstd_train_dictionary(samples bytea[], max_dict_size int4 DEFAULT 65536)
RETURNS bytea
AS 'MODULE_PATHNAME', 'pg_zstd_train_dictionary'
LANGUAGE C IMMUTABLE PARALLEL SAFE;

CREATE ENCODING TYPE IF NOT EXISTS zstd (
    KIND = 'c',
    TYPE = 'zstd',
    ENCFUNC = pg_zstd_context_encode,
    DECFUNC = pg_zstd_context_decode
);

REVOKE EXECUTE ON FUNCTION pg_zstd_context_encode(internal, anyelement) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION pg_zstd_context_decode(internal, bytea) FROM PUBLIC;
