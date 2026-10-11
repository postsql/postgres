# contrib/pg_lz4/Makefile

MODULE_big = pg_lz4
OBJS = \
	$(WIN32RES) \
	pg_lz4.o

EXTENSION = pg_lz4
DATA = pg_lz4--1.0.sql
PGFILEDESC = "pg_lz4 - LZ4 compression and dictionary training for encoding contexts"

REGRESS = pg_lz4

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_lz4
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -ldl -lm, $(LIBS))
