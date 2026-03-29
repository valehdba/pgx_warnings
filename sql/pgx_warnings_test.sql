-- Basic regression tests for pgx_warnings extension
-- Note: Full tests require shared_preload_libraries configuration

CREATE EXTENSION IF NOT EXISTS pgx_warnings;

-- Test: stats function returns proper columns
SELECT current_entries, buffer_size, total_captured, total_sent, total_failed, enabled
FROM pgx_warnings_stats();

-- Test: list function returns proper columns (empty result set)
SELECT "timestamp", level, database, message, pid, sent
FROM pgx_warnings_list(1)
LIMIT 0;

-- Test: clear function works
SELECT pgx_warnings_clear();

-- Test: convenience views exist
SELECT * FROM pgx_warnings LIMIT 0;
SELECT * FROM pgx_warnings_info;

-- Test: GUC variables are accessible
SHOW pgx_warnings.enabled;
SHOW pgx_warnings.min_elevel;
SHOW pgx_warnings.check_interval_ms;
SHOW pgx_warnings.telegram_bot_token;
SHOW pgx_warnings.telegram_chat_id;
SHOW pgx_warnings.hostname;

-- Test: test function works
SELECT pgx_warnings_test();

DROP EXTENSION pgx_warnings;
