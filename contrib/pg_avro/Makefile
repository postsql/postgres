# contrib/pg_avro/Makefile

MODULE_big = pg_avro
OBJS = \
	$(WIN32RES) \
	pg_avro.o

EXTENSION = pg_avro
DATA = pg_avro--1.0.sql
PGFILEDESC = "pg_avro - Apache Avro binary serialization for encoding contexts"

REGRESS = pg_avro

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/pg_avro
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -ldl -lm, $(LIBS))
