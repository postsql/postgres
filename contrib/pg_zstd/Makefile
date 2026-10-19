# contrib/pg_zstd/Makefile

MODULE_big = pg_zstd
OBJS = \
	$(WIN32RES) \
	pg_zstd.o

EXTENSION = pg_zstd
DATA = pg_zstd--1.0.sql
PGFILEDESC = "pg_zstd - Zstandard compression and dictionary training for encoding contexts"

REGRESS = pg_zstd

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_zstd
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lzstd -ldl -lm, $(LIBS))
