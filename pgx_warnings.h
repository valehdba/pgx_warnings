/*-------------------------------------------------------------------------
 *
 * pgx_warnings.h
 *      Public constants, data structures, and function prototypes for
 *      the pgx_warnings PostgreSQL extension.
 *
 * Copyright (c) 2026, Valeh Agayev and pgx_warnings contributors
 * Licensed under the PostgreSQL License.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGX_WARNINGS_H
#define PGX_WARNINGS_H

#include "postgres.h"
#include "storage/lwlock.h"
#include "utils/timestamp.h"

/* ========================= Constants ========================= */
#define PGX_MAX_MSG_LEN       1024
#define PGX_RING_SIZE         2048
#define PGX_URL_LEN           512
#define PGX_CHATID_LEN        128
#define PGX_HOSTNAME_LEN      128
#define PGX_DB_NAME_LEN       128
#define PGX_TELEGRAM_BUF_LEN  (PGX_MAX_MSG_LEN + 512)

/* ========================= Data Structures ========================= */

typedef struct pgxLogEntry
{
    TimestampTz ts;
    int         elevel;
    int         pid;
    char        database[PGX_DB_NAME_LEN];
    char        message[PGX_MAX_MSG_LEN];
    bool        sent;
} pgxLogEntry;

typedef struct pgxSharedState
{
    LWLock     *lock;
    int         head;           /* next write slot (wraps via modulo)  */
    int         total_count;    /* number of valid entries in buffer   */
    int         unsent_idx;     /* ring index of next entry to send    */
    int64       total_captured; /* lifetime captured count             */
    int64       total_sent;     /* lifetime sent count                 */
    int64       total_failed;   /* lifetime send failure count         */
    pgxLogEntry entries[PGX_RING_SIZE];
} pgxSharedState;

/* ========================= Function Prototypes ========================= */
void        _PG_init(void);
PGDLLEXPORT void pgx_warnings_main(Datum main_arg) pg_attribute_noreturn();

/* SQL-callable functions */
extern Datum pgx_warnings_stats(PG_FUNCTION_ARGS);
extern Datum pgx_warnings_list(PG_FUNCTION_ARGS);
extern Datum pgx_warnings_clear(PG_FUNCTION_ARGS);
extern Datum pgx_warnings_test(PG_FUNCTION_ARGS);

#endif   /* PGX_WARNINGS_H */
