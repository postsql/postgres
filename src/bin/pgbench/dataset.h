/*-------------------------------------------------------------------------
 *
 * dataset.h
 *		Shared read-only mmap dataset registry, lock-free cursors, and
 *		parallel PQputCopyData() bulk ingestion for pgbench (Pillar 1 & 5).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/dataset.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef PGBENCH_DATASET_H
#define PGBENCH_DATASET_H

#include "libpq-fe.h"
#include "pgbench.h"
#include "script.h"
#include "variable.h"

typedef enum DatasetFormat
{
	DATASET_FVECS = 0,			/* Binary float32 vectors: [int32 dim][float32 * dim] */
	DATASET_BVECS,				/* Binary uint8 vectors:   [int32 dim][uint8 * dim] */
	DATASET_IVECS,				/* Binary int32 arrays:    [int32 k][int32 * k] */
	DATASET_CSV,				/* Comma-separated values */
	DATASET_TSV					/* Tab-separated values */
} DatasetFormat;

typedef enum DatasetCursorMode
{
	CURSOR_WRAP_ROUND_ROBIN = 0,
	CURSOR_ONCE_GLOBAL
} DatasetCursorMode;

typedef struct PgBenchDataset
{
	char	   *name;			/* Logical dataset identifier */
	char	   *filepath;		/* Source file path */
	DatasetFormat format;		/* File format */
	const uint8 *raw_data;		/* Base pointer to mmap or heap buffer */
	size_t		raw_size;		/* Total mapped/allocated byte size */
	bool		is_mmapped;		/* True if backed by mmap(), false if pg_malloc */
	int64		total_rows;		/* Number of rows/vectors in dataset */
	int			dim;			/* Dimension (for FVECS/BVECS/IVECS) or column count */
	size_t		record_stride;	/* Fixed byte stride for FVECS/BVECS/IVECS, 0 for CSV/TSV */
	uint64	   *row_offsets;	/* Byte offsets of line starts for CSV/TSV (total_rows + 1) */
	uint64		cursor;			/* Lock-free global cursor for dataset_next_id / \dataset_bind */
} PgBenchDataset;

extern int	num_copy_dataset_specs;

/* CLI registration and lifecycle */
extern void registerDataset(const char *name, DatasetFormat format, const char *filepath);
extern void parseDatasetSpec(const char *spec);
extern void parseVectorDatasetSpec(const char *spec);
extern void registerCopyDatasetSpec(const char *spec);
extern void runRegisteredCopyDatasets(PGconn *con);
extern void freeAllDatasets(void);

/* Dataset lookup and row/cursor access */
extern PgBenchDataset *findDataset(const char *name);
extern int64 datasetCount(const char *name);
extern bool datasetGetRow(PgBenchDataset *ds, int64 row_idx, int col_idx,
						  PgBenchValue *retval);
extern bool datasetNextId(PgBenchDataset *ds, DatasetCursorMode mode,
						  PgBenchValue *retval);

/* Meta-command handlers */
extern ConnectionStateEnum handle_cmd_dataset_bind(CState *st, Command *cmd,
												   pg_time_usec_t *now);
extern ConnectionStateEnum handle_cmd_copy_dataset(CState *st, Command *cmd,
												   pg_time_usec_t *now);

/* Bulk COPY helper usable from pgbench -i or \copy_dataset */
extern bool copyDatasetSlice(PGconn *con, PgBenchDataset *ds,
							 const char *table_spec,
							 int client_id, int num_clients);

#endif							/* PGBENCH_DATASET_H */
