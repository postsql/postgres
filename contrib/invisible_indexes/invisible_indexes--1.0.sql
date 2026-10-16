-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION invisible_indexes" to load this file. \quit

CREATE FUNCTION version()
RETURNS text
AS 'MODULE_PATHNAME', 'invisible_indexes_version'
LANGUAGE C STRICT IMMUTABLE;

CREATE FUNCTION set_invisible(index_name text, invisible boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'invisible_indexes_set_invisible'
LANGUAGE C STRICT;

CREATE FUNCTION set_invisible(index_rel regclass, invisible boolean)
RETURNS boolean
AS 'MODULE_PATHNAME', 'invisible_indexes_set_invisible_oid'
LANGUAGE C STRICT;

CREATE FUNCTION is_invisible(index_name text)
RETURNS boolean
AS 'MODULE_PATHNAME', 'invisible_indexes_is_invisible'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION is_invisible(index_rel regclass)
RETURNS boolean
AS 'MODULE_PATHNAME', 'invisible_indexes_is_invisible_oid'
LANGUAGE C STRICT STABLE;

-- Convenience functions acting as direct ALTER INDEX analogues
CREATE FUNCTION alter_index_invisible(index_name text)
RETURNS boolean
AS $$ SELECT invisible_indexes.set_invisible($1, true); $$
LANGUAGE SQL;

CREATE FUNCTION alter_index_invisible(index_rel regclass)
RETURNS boolean
AS $$ SELECT invisible_indexes.set_invisible($1, true); $$
LANGUAGE SQL;

CREATE FUNCTION alter_index_visible(index_name text)
RETURNS boolean
AS $$ SELECT invisible_indexes.set_invisible($1, false); $$
LANGUAGE SQL;

CREATE FUNCTION alter_index_visible(index_rel regclass)
RETURNS boolean
AS $$ SELECT invisible_indexes.set_invisible($1, false); $$
LANGUAGE SQL;

-- View showing all indexes and their visibility status
CREATE VIEW invisible_indexes AS
SELECT
    n.nspname AS schemaname,
    c.relname AS indexname,
    t.relname AS tablename,
    COALESCE((d.description = 'invisible' OR d.description LIKE '%@invisible%'), false) AS is_persisted_invisible,
    invisible_indexes.is_invisible(c.oid::regclass) AS is_currently_invisible
FROM pg_class c
JOIN pg_namespace n ON n.oid = c.relnamespace
JOIN pg_index i ON i.indexrelid = c.oid
JOIN pg_class t ON t.oid = i.indrelid
LEFT JOIN pg_description d ON d.objoid = c.oid AND d.classoid = 'pg_class'::regclass AND d.objsubid = 0
WHERE c.relkind IN ('i', 'I');

-- Revoke default public permissions on administrative functions
REVOKE EXECUTE ON FUNCTION set_invisible(text, boolean) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION set_invisible(regclass, boolean) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION alter_index_invisible(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION alter_index_invisible(regclass) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION alter_index_visible(text) FROM PUBLIC;
REVOKE EXECUTE ON FUNCTION alter_index_visible(regclass) FROM PUBLIC;
