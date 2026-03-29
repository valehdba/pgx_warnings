/*-------------------------------------------------------------------------
 *
 * pgx_warnings.c
 *      PostgreSQL extension that captures log messages at WARNING level
 *      and above, stores them in a shared-memory ring buffer, and uses
 *      a background worker to dispatch Telegram notifications via libcurl.
 *
 * Copyright (c) 2026, Valeh Agayev and pgx_warnings contributors
 * Licensed under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */

#include "pgx_warnings.h"

/* System headers */
#include <curl/curl.h>
#include <unistd.h>
#include <string.h>

/* PostgreSQL headers */
#include "fmgr.h"
#include "funcapi.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/timestamp.h"

PG_MODULE_MAGIC;

/* ========================= GUC Variables ========================= */
static char *pgx_telegram_bot_token  = NULL;
static char *pgx_telegram_chat_id    = NULL;
static int   pgx_min_elevel          = WARNING;
static int   pgx_check_interval_ms   = 5000;
static bool  pgx_enabled             = true;
static char *pgx_hostname_override   = NULL;

/* ========================= Globals ========================= */
static pgxSharedState          *pgx_state                = NULL;
static emit_log_hook_type       prev_log_hook            = NULL;
static shmem_startup_hook_type  prev_shmem_startup_hook  = NULL;

/* ========================= Prototypes ========================= */
static void pgx_shmem_startup(void);
static void pgx_log_hook_fn(ErrorData *edata);
static bool pgx_send_telegram(const char *message);
static void pgx_format_message(char *buf, size_t bufsz,
                               const pgxLogEntry *entry);
static const char *pgx_elevel_string(int elevel);

/* ========================= GUC Enum ========================= */
static const struct config_enum_entry pgx_elevel_options[] = {
    {"warning", WARNING, false},
    {"error",   ERROR,   false},
    {"log",     LOG,     false},
    {"fatal",   FATAL,   false},
    {"panic",   PANIC,   false},
    {NULL, 0, false}
};

/* ========================= SQL Functions ========================= */
PG_FUNCTION_INFO_V1(pgx_warnings_stats);
PG_FUNCTION_INFO_V1(pgx_warnings_list);
PG_FUNCTION_INFO_V1(pgx_warnings_clear);
PG_FUNCTION_INFO_V1(pgx_warnings_test);

/* ================================================================
 *  cURL write callback - discard response body
 * ================================================================ */
static size_t
pgx_curl_discard(void *ptr, size_t size, size_t nmemb, void *userdata)
{
    (void) ptr;
    (void) userdata;
    return size * nmemb;
}

/* ================================================================
 *  Shared-memory size
 * ================================================================ */
static Size
pgx_shmem_size(void)
{
    return MAXALIGN(sizeof(pgxSharedState));
}

/* ================================================================
 *  Shared-memory startup hook
 * ================================================================ */
static void
pgx_shmem_startup(void)
{
    bool found;

    if (prev_shmem_startup_hook)
        prev_shmem_startup_hook();

    LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

    pgx_state = (pgxSharedState *)
        ShmemInitStruct("pgx_warnings",
                        pgx_shmem_size(),
                        &found);

    if (!found)
    {
        memset(pgx_state, 0, sizeof(pgxSharedState));
        pgx_state->lock           = &(GetNamedLWLockTranche("pgx_warnings"))->lock;
        pgx_state->head           = 0;
        pgx_state->total_count    = 0;
        pgx_state->unsent_idx     = 0;
        pgx_state->total_captured = 0;
        pgx_state->total_sent     = 0;
        pgx_state->total_failed   = 0;
    }

    LWLockRelease(AddinShmemInitLock);
}

/* ================================================================
 *  Human-readable error level
 * ================================================================ */
static const char *
pgx_elevel_string(int elevel)
{
    switch (elevel)
    {
        case LOG:     return "LOG";
        case INFO:    return "INFO";
        case NOTICE:  return "NOTICE";
        case WARNING: return "WARNING";
        case ERROR:   return "ERROR";
        case FATAL:   return "FATAL";
        case PANIC:   return "PANIC";
        default:      return "UNKNOWN";
    }
}

/* ================================================================
 *  emit_log_hook - intercept log messages
 * ================================================================ */
static void
pgx_log_hook_fn(ErrorData *edata)
{
    /* Chain previous hook first */
    if (prev_log_hook)
        prev_log_hook(edata);

    if (!pgx_enabled || !pgx_state)
        return;

    if (edata->elevel < pgx_min_elevel)
        return;

    /* Avoid capturing our own background worker messages (loop prevention) */
    if (edata->message &&
        strstr(edata->message, "pgx_warnings:") != NULL)
        return;

    LWLockAcquire(pgx_state->lock, LW_EXCLUSIVE);
    {
        int          idx   = pgx_state->head;
        pgxLogEntry *entry = &pgx_state->entries[idx];

        entry->ts     = GetCurrentTimestamp();
        entry->elevel = edata->elevel;
        entry->pid    = MyProcPid;
        entry->sent   = false;

        /* Copy database name safely */
        if (MyProcPort && MyProcPort->database_name)
            strlcpy(entry->database, MyProcPort->database_name,
                    PGX_DB_NAME_LEN);
        else
            strlcpy(entry->database, "N/A", PGX_DB_NAME_LEN);

        /* Copy message */
        if (edata->message)
            strlcpy(entry->message, edata->message, PGX_MAX_MSG_LEN);
        else
            strlcpy(entry->message, "(no message)", PGX_MAX_MSG_LEN);

        /* Advance head with proper wraparound */
        pgx_state->head = (pgx_state->head + 1) % PGX_RING_SIZE;

        /* Track buffer occupancy (capped at ring size) */
        if (pgx_state->total_count < PGX_RING_SIZE)
            pgx_state->total_count++;

        pgx_state->total_captured++;
    }
    LWLockRelease(pgx_state->lock);
}

/* ================================================================
 *  Send a single message to Telegram via libcurl
 * ================================================================ */
static bool
pgx_send_telegram(const char *message)
{
    CURL       *curl;
    CURLcode    res;
    char        url[PGX_URL_LEN];
    char       *escaped_msg  = NULL;
    char       *post_fields  = NULL;
    size_t      pf_len;
    bool        success = false;

    if (!pgx_telegram_bot_token || pgx_telegram_bot_token[0] == '\0')
        return false;
    if (!pgx_telegram_chat_id || pgx_telegram_chat_id[0] == '\0')
        return false;

    curl = curl_easy_init();
    if (!curl)
        return false;

    snprintf(url, sizeof(url),
             "https://api.telegram.org/bot%s/sendMessage",
             pgx_telegram_bot_token);

    /* Use libcurl's own URL encoding for the message */
    escaped_msg = curl_easy_escape(curl, message, 0);
    if (!escaped_msg)
    {
        curl_easy_cleanup(curl);
        return false;
    }

    pf_len = strlen(escaped_msg) + PGX_CHATID_LEN + 128;
    post_fields = (char *) malloc(pf_len);
    if (!post_fields)
    {
        curl_free(escaped_msg);
        curl_easy_cleanup(curl);
        return false;
    }

    snprintf(post_fields, pf_len,
             "chat_id=%s&parse_mode=HTML&text=%s",
             pgx_telegram_chat_id, escaped_msg);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, pgx_curl_discard);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    res = curl_easy_perform(curl);
    if (res == CURLE_OK)
    {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        success = (http_code == 200);
        if (!success)
            elog(LOG, "pgx_warnings: Telegram HTTP %ld", http_code);
    }
    else
    {
        elog(LOG, "pgx_warnings: curl error: %s", curl_easy_strerror(res));
    }

    curl_free(escaped_msg);
    free(post_fields);
    curl_easy_cleanup(curl);

    return success;
}

/* ================================================================
 *  Format a Telegram notification for one entry
 * ================================================================ */
static void
pgx_format_message(char *buf, size_t bufsz, const pgxLogEntry *entry)
{
    char        ts_str[64];
    const char *hostname;

    /*
     * Convert TimestampTz to broken-down time.
     * Pass a valid int pointer for tz offset so timestamp2tm can
     * properly handle TimestampTz (which stores UTC internally).
     */
    {
        struct pg_tm tm;
        fsec_t       fsec;
        int          tz_offset = 0;

        if (timestamp2tm(entry->ts, &tz_offset, &tm, &fsec, NULL, NULL) == 0)
            snprintf(ts_str, sizeof(ts_str),
                     "%04d-%02d-%02d %02d:%02d:%02d UTC",
                     tm.tm_year, tm.tm_mon, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
        else
            strlcpy(ts_str, "unknown-time", sizeof(ts_str));
    }

    /* Determine hostname */
    if (pgx_hostname_override && pgx_hostname_override[0] != '\0')
    {
        hostname = pgx_hostname_override;
    }
    else
    {
        static char local_hostname[PGX_HOSTNAME_LEN] = {0};
        if (local_hostname[0] == '\0')
        {
            if (gethostname(local_hostname, sizeof(local_hostname)) != 0)
                strlcpy(local_hostname, "unknown-host",
                        sizeof(local_hostname));
        }
        hostname = local_hostname;
    }

    snprintf(buf, bufsz,
             "&#x26A0; <b>PostgreSQL %s</b>\n"
             "<b>Host:</b> %s\n"
             "<b>Database:</b> %s\n"
             "<b>PID:</b> %d\n"
             "<b>Time:</b> %s\n"
             "<b>Message:</b>\n<pre>%.*s</pre>",
             pgx_elevel_string(entry->elevel),
             hostname,
             entry->database,
             entry->pid,
             ts_str,
             (int)(PGX_MAX_MSG_LEN - 1), entry->message);
}

/* ================================================================
 *  Background worker main loop
 *
 *  Uses die() as SIGTERM handler which sets ProcDiePending;
 *  CHECK_FOR_INTERRUPTS() calls proc_exit() when it fires.
 *  Uses SignalHandlerForConfigReload for SIGHUP to pick up
 *  GUC changes without restart.
 * ================================================================ */
void
pgx_warnings_main(Datum main_arg)
{
    char telegram_buf[PGX_TELEGRAM_BUF_LEN];

    /* Set up signal handlers */
    pqsignal(SIGTERM, die);
    pqsignal(SIGHUP, SignalHandlerForConfigReload);
    BackgroundWorkerUnblockSignals();

    curl_global_init(CURL_GLOBAL_ALL);

    elog(LOG, "pgx_warnings: background worker started");

    for (;;)
    {
        int rc;

        rc = WaitLatch(MyLatch,
                       WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                       pgx_check_interval_ms,
                       PG_WAIT_EXTENSION);

        ResetLatch(MyLatch);

        /* Will call proc_exit() if SIGTERM was received */
        CHECK_FOR_INTERRUPTS();

        /* Reload configuration on SIGHUP */
        if (ConfigReloadPending)
        {
            ConfigReloadPending = false;
            ProcessConfigFile(PGC_SIGHUP);
        }

        if (!pgx_enabled || !pgx_state)
            continue;

        if (!pgx_telegram_bot_token || pgx_telegram_bot_token[0] == '\0')
            continue;
        if (!pgx_telegram_chat_id || pgx_telegram_chat_id[0] == '\0')
            continue;

        /* Drain unsent entries from the ring buffer */
        for (;;)
        {
            pgxLogEntry local_copy;
            int         ring_idx;

            /*
             * Acquire EXCLUSIVE from the start so that the read of the
             * entry and the advance of unsent_idx are atomic.  This
             * eliminates the race where another backend could wrap the
             * ring buffer and overwrite the entry between a shared-lock
             * read and a later exclusive-lock update.
             */
            LWLockAcquire(pgx_state->lock, LW_EXCLUSIVE);

            if (pgx_state->unsent_idx == pgx_state->head ||
                pgx_state->total_count == 0)
            {
                LWLockRelease(pgx_state->lock);
                break;
            }

            ring_idx = pgx_state->unsent_idx;
            memcpy(&local_copy,
                   &pgx_state->entries[ring_idx],
                   sizeof(pgxLogEntry));

            /* Skip already-sent entries (possible after restart) */
            if (local_copy.sent)
            {
                pgx_state->unsent_idx =
                    (pgx_state->unsent_idx + 1) % PGX_RING_SIZE;
                LWLockRelease(pgx_state->lock);
                continue;
            }

            LWLockRelease(pgx_state->lock);

            /* Format and send (outside the lock — may block on network) */
            pgx_format_message(telegram_buf,
                               sizeof(telegram_buf),
                               &local_copy);

            if (pgx_send_telegram(telegram_buf))
            {
                LWLockAcquire(pgx_state->lock, LW_EXCLUSIVE);
                pgx_state->entries[ring_idx].sent = true;
                pgx_state->unsent_idx =
                    (pgx_state->unsent_idx + 1) % PGX_RING_SIZE;
                pgx_state->total_sent++;
                LWLockRelease(pgx_state->lock);
            }
            else
            {
                LWLockAcquire(pgx_state->lock, LW_EXCLUSIVE);
                pgx_state->total_failed++;
                LWLockRelease(pgx_state->lock);
                break;   /* retry next cycle */
            }

            CHECK_FOR_INTERRUPTS();
        }
    }

    /* Not normally reached */
    curl_global_cleanup();
    proc_exit(0);
}

/* ================================================================
 *  _PG_init - register GUCs, hooks, background worker
 * ================================================================ */
void
_PG_init(void)
{
    BackgroundWorker worker;

    if (!process_shared_preload_libraries_in_progress)
        return;

    /* ---- GUCs ---- */
    DefineCustomBoolVariable(
        "pgx_warnings.enabled",
        "Enable or disable pgx_warnings log capture.",
        NULL,
        &pgx_enabled,
        true,
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    DefineCustomStringVariable(
        "pgx_warnings.telegram_bot_token",
        "Telegram Bot API token.",
        NULL,
        &pgx_telegram_bot_token,
        "",
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    DefineCustomStringVariable(
        "pgx_warnings.telegram_chat_id",
        "Telegram chat / channel ID for notifications.",
        NULL,
        &pgx_telegram_chat_id,
        "",
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    DefineCustomEnumVariable(
        "pgx_warnings.min_elevel",
        "Minimum error level to capture.",
        NULL,
        &pgx_min_elevel,
        WARNING,
        pgx_elevel_options,
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    DefineCustomIntVariable(
        "pgx_warnings.check_interval_ms",
        "Background worker poll interval in milliseconds.",
        NULL,
        &pgx_check_interval_ms,
        5000, 500, 300000,
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    DefineCustomStringVariable(
        "pgx_warnings.hostname",
        "Override hostname shown in Telegram messages (empty = auto-detect).",
        NULL,
        &pgx_hostname_override,
        "",
        PGC_SIGHUP, 0,
        NULL, NULL, NULL);

    /* ---- Shared memory ---- */
    RequestAddinShmemSpace(pgx_shmem_size());
    RequestNamedLWLockTranche("pgx_warnings", 1);

    /* ---- Hooks ---- */
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook      = pgx_shmem_startup;

    prev_log_hook  = emit_log_hook;
    emit_log_hook  = pgx_log_hook_fn;

    /* ---- Background worker ---- */
    memset(&worker, 0, sizeof(BackgroundWorker));
    snprintf(worker.bgw_name,          BGW_MAXLEN, "pgx_warnings sender");
    snprintf(worker.bgw_type,          BGW_MAXLEN, "pgx_warnings sender");
    snprintf(worker.bgw_library_name,  BGW_MAXLEN, "pgx_warnings");
    snprintf(worker.bgw_function_name, BGW_MAXLEN, "pgx_warnings_main");
    worker.bgw_flags        = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time   = BgWorkerStart_PostmasterStart;
    worker.bgw_restart_time = 10;
    worker.bgw_main_arg     = (Datum) 0;
    worker.bgw_notify_pid   = 0;

    RegisterBackgroundWorker(&worker);

#if PG_VERSION_NUM >= 150000
    MarkGUCPrefixReserved("pgx_warnings");
#else
    EmitWarningsOnPlaceholders("pgx_warnings");
#endif
}

/* ================================================================
 *  pgx_warnings_stats() - single-row status summary
 * ================================================================ */
Datum
pgx_warnings_stats(PG_FUNCTION_ARGS)
{
    TupleDesc   tupdesc;
    Datum       values[6];
    bool        nulls[6] = {false};
    HeapTuple   tuple;

    if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
        ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("function returning record called in wrong context")));

    tupdesc = BlessTupleDesc(tupdesc);

    if (!pgx_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgx_warnings: shared memory not initialized"),
                 errhint("Add pgx_warnings to shared_preload_libraries.")));

    LWLockAcquire(pgx_state->lock, LW_SHARED);
    values[0] = Int32GetDatum(pgx_state->total_count);
    values[1] = Int32GetDatum(PGX_RING_SIZE);
    values[2] = Int64GetDatum(pgx_state->total_captured);
    values[3] = Int64GetDatum(pgx_state->total_sent);
    values[4] = Int64GetDatum(pgx_state->total_failed);
    values[5] = BoolGetDatum(pgx_enabled);
    LWLockRelease(pgx_state->lock);

    tuple = heap_form_tuple(tupdesc, values, nulls);
    PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

/* ================================================================
 *  pgx_warnings_list(limit int DEFAULT 100)
 * ================================================================ */
Datum
pgx_warnings_list(PG_FUNCTION_ARGS)
{
    FuncCallContext *funcctx;
    pgxLogEntry     *results;

    if (SRF_IS_FIRSTCALL())
    {
        MemoryContext oldctx;
        TupleDesc     tupdesc;
        int           limit, start, count, i;

        funcctx = SRF_FIRSTCALL_INIT();
        oldctx  = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

        limit = PG_GETARG_INT32(0);
        if (limit <= 0) limit = 100;
        if (limit > PGX_RING_SIZE) limit = PGX_RING_SIZE;

        if (!pgx_state)
            ereport(ERROR,
                    (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                     errmsg("pgx_warnings: shared memory not initialized")));

        results = (pgxLogEntry *) palloc0(sizeof(pgxLogEntry) * limit);

        LWLockAcquire(pgx_state->lock, LW_SHARED);

        count = pgx_state->total_count;
        if (limit > count)
            limit = count;

        start = ((pgx_state->head - limit) % PGX_RING_SIZE
                 + PGX_RING_SIZE) % PGX_RING_SIZE;

        for (i = 0; i < limit; i++)
        {
            int idx = (start + i) % PGX_RING_SIZE;
            memcpy(&results[i],
                   &pgx_state->entries[idx],
                   sizeof(pgxLogEntry));
        }

        LWLockRelease(pgx_state->lock);

        funcctx->max_calls = limit;
        funcctx->user_fctx = results;

        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in wrong context")));

        funcctx->tuple_desc = BlessTupleDesc(tupdesc);

        MemoryContextSwitchTo(oldctx);
    }

    funcctx = SRF_PERCALL_SETUP();
    results = (pgxLogEntry *) funcctx->user_fctx;

    if (funcctx->call_cntr < (uint64) funcctx->max_calls)
    {
        pgxLogEntry *e = &results[funcctx->call_cntr];
        Datum   values[6];
        bool    nulls[6] = {false};
        HeapTuple ht;

        values[0] = TimestampTzGetDatum(e->ts);
        values[1] = CStringGetTextDatum(pgx_elevel_string(e->elevel));
        values[2] = CStringGetTextDatum(e->database);
        values[3] = CStringGetTextDatum(e->message);
        values[4] = Int32GetDatum(e->pid);
        values[5] = BoolGetDatum(e->sent);

        ht = heap_form_tuple(funcctx->tuple_desc, values, nulls);
        SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(ht));
    }

    SRF_RETURN_DONE(funcctx);
}

/* ================================================================
 *  pgx_warnings_clear() - reset the ring buffer
 * ================================================================ */
Datum
pgx_warnings_clear(PG_FUNCTION_ARGS)
{
    if (!pgx_state)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("pgx_warnings: shared memory not initialized")));

    LWLockAcquire(pgx_state->lock, LW_EXCLUSIVE);
    pgx_state->head        = 0;
    pgx_state->total_count = 0;
    pgx_state->unsent_idx  = 0;
    memset(pgx_state->entries, 0, sizeof(pgx_state->entries));
    LWLockRelease(pgx_state->lock);

    PG_RETURN_VOID();
}

/* ================================================================
 *  pgx_warnings_test() - emit a test WARNING
 * ================================================================ */
Datum
pgx_warnings_test(PG_FUNCTION_ARGS)
{
    ereport(WARNING,
            (errmsg("pgx_warnings test alert: verifying capture pipeline")));

    PG_RETURN_TEXT_P(cstring_to_text(
        "Test WARNING emitted. "
        "Check pgx_warnings_list() and your Telegram channel."));
}
