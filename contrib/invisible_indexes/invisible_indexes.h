/*-------------------------------------------------------------------------
 *
 * invisible_indexes.h
 *    Definitions for invisible_indexes extension.
 *
 * Copyright (c) 2026, Google LLC.
 *
 *-------------------------------------------------------------------------
 */

#ifndef INVISIBLE_INDEXES_H
#define INVISIBLE_INDEXES_H

#include "postgres.h"
#include "fmgr.h"

#ifndef TYPALIGN_INT
#define TYPALIGN_INT 'i'
#endif

extern bool invisible_indexes_enabled;
extern bool invisible_indexes_use_invisible;
extern char *excluded_indexes_string;
extern char *invisible_indexes_excluded_string;

PGDLLEXPORT void _PG_init(void);
PGDLLEXPORT void _PG_fini(void);

PGDLLEXPORT Datum invisible_indexes_set_invisible(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum invisible_indexes_set_invisible_oid(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum invisible_indexes_is_invisible(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum invisible_indexes_is_invisible_oid(PG_FUNCTION_ARGS);
PGDLLEXPORT Datum invisible_indexes_version(PG_FUNCTION_ARGS);

#endif /* INVISIBLE_INDEXES_H */
