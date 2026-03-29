/* pgx_warnings--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pgx_warnings" to load this file. \quit

-- ---------------------------------------------------------------------------
-- pgx_warnings_stats()
--   Returns a single row with capture and notification statistics.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgx_warnings_stats(
    OUT current_entries   integer,
    OUT buffer_size       integer,
    OUT total_captured    bigint,
    OUT total_sent        bigint,
    OUT total_failed      bigint,
    OUT enabled           boolean
)
RETURNS record
AS 'MODULE_PATHNAME', 'pgx_warnings_stats'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pgx_warnings_stats() IS
    'Returns pgx_warnings buffer and notification statistics.';

-- ---------------------------------------------------------------------------
-- pgx_warnings_list(limit integer DEFAULT 100)
--   Returns entries currently in the ring buffer (most recent first).
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgx_warnings_list(
    IN  max_entries   integer DEFAULT 100,
    OUT "timestamp"   timestamptz,
    OUT level         text,
    OUT database      text,
    OUT message       text,
    OUT pid           integer,
    OUT sent          boolean
)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'pgx_warnings_list'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pgx_warnings_list(integer) IS
    'Lists captured warnings and errors from the ring buffer.';

-- ---------------------------------------------------------------------------
-- pgx_warnings_clear()
--   Clears all entries from the ring buffer.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgx_warnings_clear()
RETURNS void
AS 'MODULE_PATHNAME', 'pgx_warnings_clear'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pgx_warnings_clear() IS
    'Clears all entries from the pgx_warnings ring buffer.';

-- ---------------------------------------------------------------------------
-- pgx_warnings_test()
--   Emits a test WARNING to verify the capture and notification pipeline.
-- ---------------------------------------------------------------------------
CREATE FUNCTION pgx_warnings_test()
RETURNS text
AS 'MODULE_PATHNAME', 'pgx_warnings_test'
LANGUAGE C STRICT VOLATILE;

COMMENT ON FUNCTION pgx_warnings_test() IS
    'Emits a test WARNING to verify the pgx_warnings pipeline.';

-- ---------------------------------------------------------------------------
-- Convenience views
-- ---------------------------------------------------------------------------
CREATE VIEW pgx_warnings AS
    SELECT * FROM pgx_warnings_list(100);

COMMENT ON VIEW pgx_warnings IS
    'View of the 100 most recent captured warnings and errors.';

CREATE VIEW pgx_warnings_info AS
    SELECT * FROM pgx_warnings_stats();

COMMENT ON VIEW pgx_warnings_info IS
    'View of pgx_warnings statistics.';
