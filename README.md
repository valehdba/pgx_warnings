# pgx_warnings

A PostgreSQL extension that **captures all WARNING, ERROR, FATAL, and PANIC messages** from the server log in real time and **sends instant notifications to a Telegram channel**.

It hooks directly into PostgreSQL's `emit_log_hook` to intercept log messages before they are written to the log file, stores them in a fixed-size shared-memory ring buffer (2 048 slots), and uses a dedicated background worker process to dispatch Telegram Bot API notifications asynchronously via libcurl.

---

## Features

| Feature | Description |
|---|---|
| **Real-time capture** | Hooks directly into PostgreSQL's logging pipeline — no log-file parsing required |
| **Telegram notifications** | Sends formatted HTML messages with hostname, database, PID, timestamp, and the full error text |
| **Shared-memory ring buffer** | 2 048-entry circular buffer; zero disk I/O overhead |
| **Background worker** | Dedicated process handles all network I/O — zero impact on client sessions |
| **Loop prevention** | The extension's own log messages are excluded from capture automatically |
| **Configurable severity** | Choose the minimum level: `warning`, `error`, `log`, `fatal`, or `panic` |
| **SQL interface** | Query captured entries, view statistics, run self-tests, and clear the buffer from SQL |
| **Hot-reloadable config** | All GUC parameters respond to `pg_reload_conf()` / `SIGHUP` — no restart needed to change tokens or thresholds |
| **Lifetime statistics** | Track total captured, sent, and failed counts across the server lifetime |

---

## Architecture

```
┌──────────────────────────────────┐
│  PostgreSQL Backend Process      │
│                                  │
│  ereport(WARNING/ERROR/...)      │
│         │                        │
│         ▼                        │
│  ┌─────────────────────┐         │
│  │  emit_log_hook       │         │
│  │  (pgx_log_hook_fn)  │         │
│  └────────┬────────────┘         │
│           │  LWLock              │
│           ▼                      │
│  ┌─────────────────────┐         │
│  │  Shared Memory       │         │
│  │  Ring Buffer (2048)  │         │
│  └────────┬────────────┘         │
└───────────┼──────────────────────┘
            │
            │  LWLock
            ▼
┌──────────────────────────────────┐
│  Background Worker               │
│  (pgx_warnings_main)            │
│         │                        │
│         │  SIGHUP → reload GUCs  │
│         │  SIGTERM → graceful    │
│         │           shutdown     │
│         ▼                        │
│  ┌─────────────────────┐         │
│  │  libcurl → Telegram  │         │
│  │  Bot API (HTTPS)     │         │
│  └─────────────────────┘         │
└──────────────────────────────────┘
```

---

## Prerequisites

Before building, make sure the following packages are installed:

| Component | Why it's needed |
|---|---|
| PostgreSQL 13 – 17 | Server headers and PGXS build system |
| Development headers | `pg_config` must be on PATH |
| `libcurl` dev package | HTTP client for Telegram Bot API calls |
| `gcc` / `make` | C compiler and build tool |
| `pkg-config` | Used by the Makefile to locate libcurl flags |

### Install dependencies on Debian / Ubuntu

```bash
# Replace 16 with your PostgreSQL major version
sudo apt-get update
sudo apt-get install -y \
    postgresql-server-dev-16 \
    libcurl4-openssl-dev \
    gcc \
    make \
    pkg-config
```

### Install dependencies on RHEL / CentOS / Rocky Linux / AlmaLinux

```bash
# Replace 16 with your PostgreSQL major version
sudo dnf install -y \
    postgresql16-devel \
    libcurl-devel \
    gcc \
    make \
    pkgconfig
```

### Install dependencies on macOS (Homebrew)

```bash
brew install postgresql libcurl pkg-config
```

---

## Step-by-Step Installation

### Step 1 — Clone the Repository

```bash
git clone https://github.com/valehdba/pgx_warnings.git
cd pgx_warnings
```

### Step 2 — Build the Extension

```bash
make USE_PGXS=1
```

Expected output:

```
gcc -Wall -Wmissing-prototypes ... -c -o pgx_warnings.o pgx_warnings.c
gcc -shared -o pgx_warnings.so pgx_warnings.o -lcurl
```

If `pg_config` is not on your `PATH`, specify it explicitly:

```bash
make USE_PGXS=1 PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config
```

### Step 3 — Install the Extension Files

```bash
sudo make install USE_PGXS=1
```

This copies three files into the PostgreSQL tree:

| File | Destination |
|---|---|
| `pgx_warnings.so` | `$(pg_config --pkglibdir)/` |
| `pgx_warnings.control` | `$(pg_config --sharedir)/extension/` |
| `pgx_warnings--1.0.sql` | `$(pg_config --sharedir)/extension/` |

### Step 4 — Create a Telegram Bot and Get Credentials

1. Open Telegram and search for **@BotFather**.
2. Send `/newbot` and follow the prompts to choose a name.
3. BotFather will reply with your **Bot Token** (e.g. `123456789:ABCDefGHIjklMNOpqrsTUVwxyz`) — save it.
4. Create a Telegram **channel** or **group** where alerts will be posted.
5. Add the bot as an **administrator** of that channel/group.
6. Obtain the **Chat ID**:
   - **Public channels:** use `@channelname` (e.g. `@my_pg_alerts`).
   - **Private channels/groups:** send any message in the chat, then visit:
     ```
     https://api.telegram.org/bot<YOUR_BOT_TOKEN>/getUpdates
     ```
     Find `"chat":{"id": -100XXXXXXXXXX}` in the JSON response — that number (including the minus sign) is your Chat ID.

### Step 5 — Configure PostgreSQL

Edit your `postgresql.conf` (find it with `SHOW config_file;`):

```ini
# ── Load the extension (requires restart) ───────────────────────
shared_preload_libraries = 'pgx_warnings'

# ── Telegram credentials ────────────────────────────────────────
pgx_warnings.telegram_bot_token = '123456789:ABCDefGHIjklMNOpqrsTUVwxyz'
pgx_warnings.telegram_chat_id   = '-1001234567890'

# ── Optional tuning (all can be changed at runtime via SIGHUP) ──
pgx_warnings.enabled            = on          # master on/off switch
pgx_warnings.min_elevel         = 'warning'   # warning | error | log | fatal | panic
pgx_warnings.check_interval_ms  = 5000        # background worker poll interval (ms)
pgx_warnings.hostname           = 'prod-db-01'  # label shown in messages (auto-detected if empty)
```

### Step 6 — Restart PostgreSQL

The first load requires a full restart because the extension allocates shared memory and starts a background worker:

```bash
sudo systemctl restart postgresql
```

### Step 7 — Create the Extension in Your Database(s)

Connect to each database where you want the SQL interface:

```sql
CREATE EXTENSION pgx_warnings;
```

> **Note:** The background worker and log hook are active server-wide regardless of which databases have the extension created. The `CREATE EXTENSION` step only installs the SQL functions and views.

### Step 8 — Verify Everything Works

```sql
-- 1. Emit a test warning (should appear on Telegram within a few seconds)
SELECT pgx_warnings_test();

-- 2. Check statistics
SELECT * FROM pgx_warnings_info;

-- 3. List captured entries
SELECT * FROM pgx_warnings;

-- 4. Or call the function directly with a custom limit
SELECT * FROM pgx_warnings_list(10);
```

You should receive a Telegram message like:

```
⚠ PostgreSQL WARNING
Host: prod-db-01
Database: mydb
PID: 12345
Time: 2026-03-29 14:22:05 UTC
Message:
pgx_warnings test alert: verifying capture pipeline
```

---

## Configuration Reference

All parameters except `shared_preload_libraries` can be changed at runtime with `ALTER SYSTEM` followed by `SELECT pg_reload_conf();`.

| Parameter | Type | Default | Range | Description |
|---|---|---|---|---|
| `pgx_warnings.enabled` | bool | `on` | — | Master on/off switch for capture |
| `pgx_warnings.telegram_bot_token` | string | `''` | — | Telegram Bot API token from @BotFather |
| `pgx_warnings.telegram_chat_id` | string | `''` | — | Target chat/channel/group ID |
| `pgx_warnings.min_elevel` | enum | `warning` | `log`, `warning`, `error`, `fatal`, `panic` | Minimum severity to capture |
| `pgx_warnings.check_interval_ms` | int | `5000` | 500 – 300 000 | Background worker poll interval (ms) |
| `pgx_warnings.hostname` | string | `''` | — | Override hostname in messages (auto-detected if empty) |

### Example: Change settings at runtime

```sql
ALTER SYSTEM SET pgx_warnings.min_elevel = 'error';
ALTER SYSTEM SET pgx_warnings.check_interval_ms = 2000;
SELECT pg_reload_conf();
```

---

## SQL Functions and Views

### `pgx_warnings_stats()`

Returns a single row with buffer and notification statistics.

```sql
SELECT * FROM pgx_warnings_stats();
-- Or use the convenience view:
SELECT * FROM pgx_warnings_info;
```

| Column | Type | Description |
|---|---|---|
| `current_entries` | integer | Entries currently in the ring buffer |
| `buffer_size` | integer | Maximum ring buffer capacity (2048) |
| `total_captured` | integer | Lifetime count of captured messages |
| `total_sent` | integer | Lifetime count of Telegram notifications sent |
| `total_failed` | integer | Lifetime count of send failures |
| `enabled` | boolean | Whether capture is currently active |

### `pgx_warnings_list(max_entries integer DEFAULT 100)`

Returns the most recent entries from the ring buffer.

```sql
SELECT * FROM pgx_warnings_list(20);
-- Or use the convenience view (returns last 100):
SELECT * FROM pgx_warnings;
```

| Column | Type | Description |
|---|---|---|
| `timestamp` | timestamptz | When the message was captured |
| `level` | text | Severity level (WARNING, ERROR, FATAL, PANIC) |
| `database` | text | Database where the message originated |
| `message` | text | The log message text |
| `pid` | integer | Backend process ID that generated the message |
| `sent` | boolean | Whether Telegram notification was dispatched |

### `pgx_warnings_clear()`

Resets the ring buffer and clears all captured entries. Lifetime counters are preserved.

```sql
SELECT pgx_warnings_clear();
```

### `pgx_warnings_test()`

Emits a test `WARNING` to verify the full capture → Telegram pipeline end to end.

```sql
SELECT pgx_warnings_test();
```

### Views

| View | Description |
|---|---|
| `pgx_warnings` | Shortcut for `SELECT * FROM pgx_warnings_list(100)` |
| `pgx_warnings_info` | Shortcut for `SELECT * FROM pgx_warnings_stats()` |

---

## Monitoring and Operations

### Check the background worker is running

```sql
SELECT pid, state, backend_type
  FROM pg_stat_activity
 WHERE backend_type = 'pgx_warnings sender';
```

### Check PostgreSQL log for startup confirmation

```bash
grep "pgx_warnings" /var/log/postgresql/postgresql-16-main.log
```

Expected output:

```
LOG:  pgx_warnings: background worker started
```

### Temporarily disable capture and notifications

```sql
ALTER SYSTEM SET pgx_warnings.enabled = off;
SELECT pg_reload_conf();
```

### Re-enable

```sql
ALTER SYSTEM SET pgx_warnings.enabled = on;
SELECT pg_reload_conf();
```

### Monitor notification health

```sql
SELECT total_captured, total_sent, total_failed
  FROM pgx_warnings_info;
```

If `total_failed` is increasing, check:
- Bot token validity
- Chat ID correctness
- Network connectivity from the database server to `api.telegram.org`
- PostgreSQL server logs for `pgx_warnings: curl error:` or `pgx_warnings: Telegram HTTP` messages

---

## Uninstalling

### Step 1 — Drop the extension from each database

```sql
DROP EXTENSION pgx_warnings;
```

### Step 2 — Remove from `shared_preload_libraries`

Edit `postgresql.conf` and remove `pgx_warnings` from the `shared_preload_libraries` line.

### Step 3 — Restart PostgreSQL

```bash
sudo systemctl restart postgresql
```

### Step 4 — Remove installed files (optional)

```bash
sudo make uninstall USE_PGXS=1
```

---

## Troubleshooting

| Problem | Solution |
|---|---|
| **No Telegram messages received** | Verify `telegram_bot_token` and `telegram_chat_id` are set correctly. Confirm the bot is an admin of the target channel. Check PostgreSQL logs for `curl error` or `Telegram HTTP` messages. |
| **Extension fails to load** | Confirm `pgx_warnings` is listed in `shared_preload_libraries` and PostgreSQL was **restarted** (reload is not enough for the first load). |
| **`pgx_warnings.so: cannot open shared object file`** | Run `sudo make install USE_PGXS=1` and verify the `.so` exists in `$(pg_config --pkglibdir)`. |
| **Build error: `curl/curl.h: No such file or directory`** | Install the libcurl development package: `libcurl4-openssl-dev` (Debian/Ubuntu) or `libcurl-devel` (RHEL/CentOS). |
| **HTTP 401 from Telegram** | Bot token is invalid or expired. Regenerate it via @BotFather. |
| **HTTP 400 from Telegram** | Chat ID is incorrect. Re-check using the `/getUpdates` method described in Step 4. |
| **`total_failed` keeps increasing** | The database server cannot reach `api.telegram.org`. Check DNS resolution, firewall rules, and outbound HTTPS (port 443) connectivity. |
| **Ring buffer full / old entries missing** | The buffer is circular — oldest entries are overwritten when full. Increase the poll frequency (`check_interval_ms`) or address Telegram delivery failures so entries are sent before being overwritten. |
| **`shared memory not initialized` error** | The extension is not loaded via `shared_preload_libraries`. Add it and restart PostgreSQL. |

---

## Compatibility

| PostgreSQL Version | Status |
|---|---|
| 17 | ✅ Supported |
| 16 | ✅ Supported |
| 15 | ✅ Supported |
| 14 | ✅ Supported |
| 13 | ✅ Supported |
| 12 and earlier | ❌ Not tested |

---

## Author

**Valeh Agayev** — [valeh.agayev@gmail.com](mailto:valeh.agayev@gmail.com)

## License

Released under the [PostgreSQL License](LICENSE).
