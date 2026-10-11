# contrib/pg_cascade/Makefile

MODULE_big = pg_cascade
OBJS = \
	$(WIN32RES) \
	pg_cascade.o

EXTENSION = pg_cascade
DATA = pg_cascade--1.0.sql
PGFILEDESC = "pg_cascade - multi-stage cascade and fused Avro+Zstd encoding context handlers"

REGRESS = pg_cascade
EXTRA_INSTALL = contrib/pg_avro contrib/pg_zstd contrib/pg_lz4

SHLIB_LINK += $(filter -ldl -lm, $(LIBS))

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_cascade
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
