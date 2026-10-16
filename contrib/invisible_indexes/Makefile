# contrib/invisible_indexes/Makefile

MODULE_big = invisible_indexes
OBJS = invisible_indexes.o $(WIN32RES)

EXTENSION = invisible_indexes
DATA = invisible_indexes--1.0.sql
PGFILEDESC = "invisible_indexes - support for invisible and excluded indexes"

REGRESS = invisible_indexes
REGRESS_OPTS = --temp-config invisible_indexes_regression_test_settings.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/invisible_indexes
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
