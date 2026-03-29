MODULE_big   = pgx_warnings
OBJS         = pgx_warnings.o

EXTENSION    = pgx_warnings
DATA         = pgx_warnings--1.0.sql
PGFILEDESC   = "pgx_warnings - capture log warnings/errors and notify Telegram"

PG_CPPFLAGS  = $(shell pkg-config --cflags libcurl 2>/dev/null || curl-config --cflags 2>/dev/null)
SHLIB_LINK   = $(shell pkg-config --libs   libcurl 2>/dev/null || curl-config --libs   2>/dev/null)

ifdef USE_PGXS
PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pgx_warnings
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
