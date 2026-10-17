/*-------------------------------------------------------------------------
 *
 * dataset.c
 *		Shared read-only mmap dataset registry, lock-free cursors, and
 *		parallel PQputCopyData() bulk ingestion for pgbench (Pillar 1 & 5).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/bin/pgbench/dataset.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres_fe.h"

#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef WIN32
#include <sys/mman.h>
#endif

#include "commands.h"
#include "common/logging.h"
#include "common/pgbench_funcs.h"
#include "dataset.h"
#include "engine.h"
#include "fe_utils/string_utils.h"
#include "pqexpbuffer.h"

#define MAX_DATASETS 64
#define MAX_COPY_SPECS 64
#define COPY_CHUNK_BYTES (64 * 1024)

static PgBenchDataset datasets[MAX_DATASETS];
static int	num_datasets = 0;
static bool atexit_registered = false;

typedef struct CopyDatasetSpec
{
	char	   *dataset_name;
	char	   *table_spec;
} CopyDatasetSpec;

static CopyDatasetSpec copy_specs[MAX_COPY_SPECS];
int			num_copy_dataset_specs = 0;

/*
 * Cleanup all registered datasets at process exit.
 * Datasets remain mapped for the entire process lifetime so that borrowed
 * PGBT_VECTOR / PGBT_BYTEA pointers in client variables never dangle.
 */
void
freeAllDatasets(void)
{
	for (int i = 0; i < num_datasets; i++)
	{
		PgBenchDataset *ds = &datasets[i];

		if (ds->raw_data != NULL)
		{
#ifndef WIN32
			if (ds->is_mmapped)
				(void) munmap(unconstify(uint8 *, ds->raw_data), ds->raw_size);
			else
#endif
				pg_free(unconstify(uint8 *, ds->raw_data));
			ds->raw_data = NULL;
		}
		pg_free(ds->row_offsets);
		ds->row_offsets = NULL;
		pg_free(ds->name);
		ds->name = NULL;
		pg_free(ds->filepath);
		ds->filepath = NULL;
	}
	num_datasets = 0;

	for (int i = 0; i < num_copy_dataset_specs; i++)
	{
		pg_free(copy_specs[i].dataset_name);
		pg_free(copy_specs[i].table_spec);
	}
	num_copy_dataset_specs = 0;
}

/*
 * Find a loaded dataset by logical name.
 */
PgBenchDataset *
findDataset(const char *name)
{
	if (name == NULL)
		return NULL;

	for (int i = 0; i < num_datasets; i++)
	{
		if (strcmp(datasets[i].name, name) == 0)
			return &datasets[i];
	}
	return NULL;
}

/*
 * Return the row count of a dataset by name, or -1 if not found.
 */
int64
datasetCount(const char *name)
{
	PgBenchDataset *ds = findDataset(name);

	if (ds == NULL)
		return -1;
	return ds->total_rows;
}

/*
 * Build line-start offset index for CSV/TSV datasets.
 */
static void
indexDelimitedDataset(PgBenchDataset *ds)
{
	const uint8 *data = ds->raw_data;
	size_t		size = ds->raw_size;
	int64		cap = 1024;
	int64		nrows = 0;
	uint64	   *offsets;
	size_t		pos = 0;

	offsets = pg_malloc_array(uint64, cap + 1);

	while (pos < size)
	{
		size_t		line_start = pos;
		size_t		line_end;

		while (pos < size && data[pos] != '\n')
			pos++;
		line_end = pos;
		if (pos < size && data[pos] == '\n')
			pos++;

		/* Ignore empty trailing lines */
		if (line_end > line_start && data[line_end - 1] == '\r')
			line_end--;
		if (line_end == line_start)
			continue;

		if (nrows >= cap)
		{
			cap *= 2;
			offsets = pg_realloc_array(offsets, uint64, cap + 1);
		}
		offsets[nrows++] = (uint64) line_start;
	}
	offsets[nrows] = (uint64) size;

	/*
	 * If there are at least 2 rows and the first row starts with an
	 * alphabetic header identifier while the second row starts with a digit
	 * or sign, treat the first row as a header line and skip it.
	 */
	if (nrows >= 2)
	{
		uint8		c0 = data[offsets[0]];
		uint8		c1 = data[offsets[1]];

		if (((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_') &&
			((c1 >= '0' && c1 <= '9') || c1 == '-' || c1 == '+'))
		{
			memmove(offsets, offsets + 1, (size_t) nrows * sizeof(uint64));
			nrows--;
		}
	}

	ds->row_offsets = offsets;
	ds->total_rows = nrows;

	/* Count columns in the first row if non-empty */
	if (nrows > 0)
	{
		char		delim = (ds->format == DATASET_TSV) ? '\t' : ',';
		uint64		start = offsets[0];
		uint64		end = (nrows > 1) ? offsets[1] : (uint64) size;
		int			cols = 1;
		bool		in_quotes = false;

		/* Auto-detect pipe delimiter for LDBC-style CSV files without commas */
		if (ds->format == DATASET_CSV)
		{
			bool		has_comma = false;
			bool		has_pipe = false;

			for (uint64 i = start; i < end && data[i] != '\n' && data[i] != '\r'; i++)
			{
				if (data[i] == ',')
					has_comma = true;
				else if (data[i] == '|')
					has_pipe = true;
			}
			if (!has_comma && has_pipe)
				delim = '|';
		}

		for (uint64 i = start; i < end && data[i] != '\n' && data[i] != '\r'; i++)
		{
			if (data[i] == '"')
				in_quotes = !in_quotes;
			else if (!in_quotes && data[i] == (uint8) delim)
				cols++;
		}
		ds->dim = cols;
	}
	else
	{
		ds->dim = 0;
	}
	ds->record_stride = 0;
}

/*
 * Validate and compute stride/row count for binary vector/ivec files.
 */
static void
validateBinaryVectorDataset(PgBenchDataset *ds)
{
	int32		first_dim = 0;
	int32		last_dim = 0;
	size_t		elem_size;
	size_t		stride;

	if (ds->raw_size == 0)
	{
		ds->dim = 0;
		ds->record_stride = 0;
		ds->total_rows = 0;
		return;
	}

	if (ds->raw_size < sizeof(int32))
		pg_fatal("dataset \"%s\" (%s): file too small (%zu bytes)",
				 ds->name, ds->filepath, ds->raw_size);

	memcpy(&first_dim, ds->raw_data, sizeof(int32));
	if (first_dim <= 0 || first_dim > PGBENCH_MAX_VECTOR_DIM)
		pg_fatal("dataset \"%s\" (%s): invalid vector dimension %d (must be 1..%d)",
				 ds->name, ds->filepath, (int) first_dim, PGBENCH_MAX_VECTOR_DIM);

	elem_size = (ds->format == DATASET_BVECS) ? sizeof(uint8) : sizeof(float);
	stride = sizeof(int32) + (size_t) first_dim * elem_size;

	if ((ds->raw_size % stride) != 0)
		pg_fatal("dataset \"%s\" (%s): file size %zu is not a multiple of record stride %zu (dim=%d)",
				 ds->name, ds->filepath, ds->raw_size, stride, (int) first_dim);

	ds->dim = (int) first_dim;
	ds->record_stride = stride;
	ds->total_rows = (int64) (ds->raw_size / stride);

	/* Verify the final record's dimension header matches first_dim */
	if (ds->total_rows > 1)
	{
		const uint8 *last_rec = ds->raw_data + (size_t) (ds->total_rows - 1) * stride;

		memcpy(&last_dim, last_rec, sizeof(int32));
		if (last_dim != first_dim)
			pg_fatal("dataset \"%s\" (%s): inconsistent dimension in last record (%d != %d)",
					 ds->name, ds->filepath, (int) last_dim, (int) first_dim);
	}
}

/*
 * Load and register a dataset in the shared read-only dataset pool.
 */
void
registerDataset(const char *name, DatasetFormat format, const char *filepath)
{
	int			fd;
	struct stat st;
	PgBenchDataset *ds;
	uint8	   *buf = NULL;
	bool		mmapped = false;

	if (name == NULL || *name == '\0')
		pg_fatal("dataset name cannot be empty");
	if (filepath == NULL || *filepath == '\0')
		pg_fatal("dataset \"%s\": file path cannot be empty", name);

	if (findDataset(name) != NULL)
		pg_fatal("duplicate dataset name \"%s\"", name);

	if (num_datasets >= MAX_DATASETS)
		pg_fatal("too many datasets registered (maximum %d)", MAX_DATASETS);

	if (!atexit_registered)
	{
		atexit(freeAllDatasets);
		atexit_registered = true;
	}

	fd = open(filepath, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
		pg_fatal("could not open dataset \"%s\" file \"%s\": %m", name, filepath);

	if (fstat(fd, &st) < 0)
	{
		close(fd);
		pg_fatal("could not stat dataset \"%s\" file \"%s\": %m", name, filepath);
	}

	if (st.st_size > 0)
	{
#ifndef WIN32
		/*
		 * Use read-only shared mmap on 64-bit platforms, falling back to
		 * heap allocation if mmap fails or on 32-bit platforms.
		 */
		if (sizeof(void *) >= 8)
		{
			void	   *mapped = mmap(NULL, (size_t) st.st_size, PROT_READ,
									  MAP_SHARED, fd, 0);

			if (mapped != MAP_FAILED)
			{
#ifdef MADV_WILLNEED
				(void) madvise(mapped, (size_t) st.st_size, MADV_WILLNEED);
#endif
				buf = (uint8 *) mapped;
				mmapped = true;
			}
		}
#endif
		if (!mmapped)
		{
			size_t		total_read = 0;
			size_t		to_read = (size_t) st.st_size;

			buf = (uint8 *) pg_malloc(to_read);
			while (total_read < to_read)
			{
				ssize_t		r = read(fd, buf + total_read, to_read - total_read);

				if (r < 0)
				{
					if (errno == EINTR)
						continue;
					close(fd);
					pg_free(buf);
					pg_fatal("could not read dataset \"%s\" file \"%s\": %m",
							 name, filepath);
				}
				if (r == 0)
					break;
				total_read += (size_t) r;
			}
			if (total_read != to_read)
			{
				close(fd);
				pg_free(buf);
				pg_fatal("short read on dataset \"%s\" file \"%s\" (got %zu of %zu bytes)",
						 name, filepath, total_read, to_read);
			}
		}
	}
	close(fd);

	ds = &datasets[num_datasets++];
	memset(ds, 0, sizeof(PgBenchDataset));
	ds->name = pg_strdup(name);
	ds->filepath = pg_strdup(filepath);
	ds->format = format;
	ds->raw_data = buf;
	ds->raw_size = (size_t) st.st_size;
	ds->is_mmapped = mmapped;
	__atomic_store_n(&ds->cursor, 0, __ATOMIC_RELAXED);

	if (format == DATASET_FVECS || format == DATASET_BVECS || format == DATASET_IVECS)
		validateBinaryVectorDataset(ds);
	else
		indexDelimitedDataset(ds);

	pg_log_info("loaded dataset \"%s\" (%s): " INT64_FORMAT " rows, dim/cols=%d (%s)",
				ds->name, ds->filepath, ds->total_rows, ds->dim,
				ds->is_mmapped ? "mmap" : "heap");
}

/*
 * Parse a "--dataset=<name>[:<format>]=<path>" or "--dataset=<name>:<format>:<path>"
 * or "--dataset=<name>=<path>" or "--dataset=<name>:<path>" specification.
 */
void
parseDatasetSpec(const char *spec)
{
	const char *sep1 = NULL;
	const char *rest;
	const char *sep2 = NULL;
	char	   *name;
	const char *filepath;
	DatasetFormat fmt = DATASET_FVECS;
	bool		explicit_fmt = false;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --dataset specification: empty string");

	for (const char *p = spec; *p != '\0'; p++)
	{
		if (*p == ':' || *p == '=')
		{
			sep1 = p;
			break;
		}
	}

	if (sep1 == NULL || sep1 == spec || *(sep1 + 1) == '\0')
		pg_fatal("invalid --dataset specification \"%s\" (expected <name>[:<format>]=<path> or <name>:<format>:<path>)",
				 spec);

	name = pg_malloc((sep1 - spec) + 1);
	memcpy(name, spec, sep1 - spec);
	name[sep1 - spec] = '\0';

	rest = sep1 + 1;
	for (const char *p = rest; *p != '\0'; p++)
	{
		if (*p == ':' || *p == '=')
		{
			sep2 = p;
			break;
		}
		if (*p == '/')
			break;
	}

	if (sep2 != NULL && sep2 > rest && *(sep2 + 1) != '\0')
	{
		char	   *fmt_str = pg_malloc((sep2 - rest) + 1);

		memcpy(fmt_str, rest, sep2 - rest);
		fmt_str[sep2 - rest] = '\0';

		if (pg_strcasecmp(fmt_str, "fvecs") == 0)
			fmt = DATASET_FVECS;
		else if (pg_strcasecmp(fmt_str, "bvecs") == 0)
			fmt = DATASET_BVECS;
		else if (pg_strcasecmp(fmt_str, "ivecs") == 0)
			fmt = DATASET_IVECS;
		else if (pg_strcasecmp(fmt_str, "csv") == 0 ||
				 pg_strcasecmp(fmt_str, "psv") == 0 ||
				 pg_strcasecmp(fmt_str, "pipe") == 0)
			fmt = DATASET_CSV;
		else if (pg_strcasecmp(fmt_str, "tsv") == 0)
			fmt = DATASET_TSV;
		else
			pg_fatal("unknown dataset format \"%s\" in --dataset=\"%s\" (expected fvecs, bvecs, ivecs, csv, or tsv)",
					 fmt_str, spec);

		pg_free(fmt_str);
		filepath = sep2 + 1;
		explicit_fmt = true;
	}
	else
	{
		filepath = rest;
	}

	if (!explicit_fmt)
	{
		const char *ext = strrchr(filepath, '.');

		if (ext != NULL)
		{
			if (pg_strcasecmp(ext, ".fvecs") == 0)
				fmt = DATASET_FVECS;
			else if (pg_strcasecmp(ext, ".bvecs") == 0)
				fmt = DATASET_BVECS;
			else if (pg_strcasecmp(ext, ".ivecs") == 0)
				fmt = DATASET_IVECS;
			else if (pg_strcasecmp(ext, ".csv") == 0)
				fmt = DATASET_CSV;
			else if (pg_strcasecmp(ext, ".tsv") == 0)
				fmt = DATASET_TSV;
		}
	}

	registerDataset(name, fmt, filepath);
	pg_free(name);
}

/*
 * Alias for parseDatasetSpec().
 */
void
parseVectorDatasetSpec(const char *spec)
{
	parseDatasetSpec(spec);
}

/*
 * Register a "--copy-dataset=<dataset>:<table>[(cols)]" specification.
 */
void
registerCopyDatasetSpec(const char *spec)
{
	const char *colon;
	char	   *ds_name;
	const char *table_spec;

	if (spec == NULL || *spec == '\0')
		pg_fatal("invalid --copy-dataset specification: empty string");

	colon = strchr(spec, ':');
	if (colon == NULL || colon == spec || *(colon + 1) == '\0')
		pg_fatal("invalid --copy-dataset specification \"%s\" (expected <dataset>:<table>[(cols)])",
				 spec);

	if (num_copy_dataset_specs >= MAX_COPY_SPECS)
		pg_fatal("too many --copy-dataset specifications (maximum %d)", MAX_COPY_SPECS);

	ds_name = pg_malloc((colon - spec) + 1);
	memcpy(ds_name, spec, colon - spec);
	ds_name[colon - spec] = '\0';
	table_spec = colon + 1;

	copy_specs[num_copy_dataset_specs].dataset_name = ds_name;
	copy_specs[num_copy_dataset_specs].table_spec = pg_strdup(table_spec);
	num_copy_dataset_specs++;
}

/*
 * Parse a scalar token from a CSV/TSV cell into a PgBenchValue.
 */
static void
parseDelimitedCellValue(const char *token, bool was_quoted, PgBenchValue *val)
{
	size_t		len = strlen(token);
	int64		ival;
	double		dval;

	if (!was_quoted && (len == 0 || pg_strcasecmp(token, "NULL") == 0 ||
						strcmp(token, "\\N") == 0))
	{
		setNullValue(val);
		return;
	}

	if (len >= 3 && token[0] == '[' && token[len - 1] == ']' &&
		parseVectorLiteral(token, val))
		return;

	if (len >= 2 && token[0] == '{' && token[len - 1] == '}' &&
		parseArrayLiteral(token, val))
		return;

	if (len >= 2 && token[0] == '\\' && (token[1] == 'x' || token[1] == 'X') &&
		parseByteaLiteral(token, val))
		return;

	if (!was_quoted)
	{
		if (pg_strcasecmp(token, "true") == 0)
		{
			setBoolValue(val, true);
			return;
		}
		if (pg_strcasecmp(token, "false") == 0)
		{
			setBoolValue(val, false);
			return;
		}
		if (is_an_int(token) && strtoint64(token, true, &ival))
		{
			setIntValue(val, ival);
			return;
		}
		if (strtodouble(token, true, &dval))
		{
			setDoubleValue(val, dval);
			return;
		}
	}

	setTextValue(val, token);
}

/*
 * Parse a row from a CSV/TSV dataset into an array of column values, or extract
 * a single column when target_col >= 0.
 */
static bool
parseDelimitedRow(PgBenchDataset *ds, int64 row_idx, int target_col,
				  PgBenchValue *retval)
{
	uint64		start = ds->row_offsets[row_idx];
	uint64		end = ds->row_offsets[row_idx + 1];
	const char *p = (const char *) (ds->raw_data + start);
	const char *line_end = (const char *) (ds->raw_data + end);
	char		delim = (ds->format == DATASET_TSV) ? '\t' : ',';
	PgBenchValue *cols = NULL;
	int			ncols = 0;
	int			cap = 0;

	/* Strip trailing CR/LF */
	while (line_end > p && (line_end[-1] == '\n' || line_end[-1] == '\r'))
		line_end--;

	/* Auto-detect pipe delimiter if CSV line has '|' and no ',' */
	if (ds->format == DATASET_CSV)
	{
		bool		has_comma = false;
		bool		has_pipe = false;

		for (const char *scan = p; scan < line_end; scan++)
		{
			if (*scan == ',')
				has_comma = true;
			else if (*scan == '|')
				has_pipe = true;
		}
		if (!has_comma && has_pipe)
			delim = '|';
	}

	while (p <= line_end)
	{
		PQExpBufferData cell;
		bool		was_quoted = false;
		PgBenchValue cell_val;

		initPQExpBuffer(&cell);

		if (p < line_end && *p == '"')
		{
			was_quoted = true;
			p++;
			while (p < line_end)
			{
				if (*p == '"')
				{
					if ((p + 1) < line_end && p[1] == '"')
					{
						appendPQExpBufferChar(&cell, '"');
						p += 2;
					}
					else
					{
						p++;
						break;
					}
				}
				else
				{
					appendPQExpBufferChar(&cell, *p++);
				}
			}
			while (p < line_end && *p != delim)
				p++;
		}
		else
		{
			const char *tok_start = p;

			while (p < line_end && *p != delim)
				p++;
			appendBinaryPQExpBuffer(&cell, tok_start, p - tok_start);
		}

		if (target_col >= 0)
		{
			if (ncols == target_col)
			{
				parseDelimitedCellValue(cell.data, was_quoted, retval);
				termPQExpBuffer(&cell);
				return true;
			}
			termPQExpBuffer(&cell);
			ncols++;
		}
		else
		{
			parseDelimitedCellValue(cell.data, was_quoted, &cell_val);
			termPQExpBuffer(&cell);

			if (ncols >= cap)
			{
				cap = (cap == 0) ? 8 : cap * 2;
				cols = pg_realloc_array(cols, PgBenchValue, cap);
			}
			cols[ncols++] = cell_val;
		}

		if (p < line_end && *p == delim)
		{
			p++;
			/* Handle trailing empty cell after final delimiter */
			if (p == line_end)
			{
				if (target_col >= 0)
				{
					if (ncols == target_col)
					{
						setNullValue(retval);
						return true;
					}
					ncols++;
				}
				else
				{
					if (ncols >= cap)
					{
						cap = (cap == 0) ? 8 : cap * 2;
						cols = pg_realloc_array(cols, PgBenchValue, cap);
					}
					setNullValue(&cols[ncols++]);
				}
				break;
			}
		}
		else
			break;
	}

	if (target_col >= 0)
	{
		pg_log_error("dataset \"%s\": column index %d out of range (row " INT64_FORMAT " has %d columns)",
					 ds->name, target_col, row_idx, ncols);
		return false;
	}

	if (ncols == 1)
	{
		*retval = cols[0];
		pg_free(cols);
	}
	else
	{
		retval->type = PGBT_ARRAY;
		retval->u.aval.nelems = ncols;
		retval->u.aval.elems = cols;
	}
	return true;
}

/*
 * Retrieve a row or cell from a dataset in O(1) time.
 * When col_idx < 0:
 *   - DATASET_FVECS: returns zero-copy borrowed PGBT_VECTOR
 *   - DATASET_BVECS: returns heap-allocated PGBT_VECTOR
 *   - DATASET_IVECS: returns PGBT_ARRAY of PGBT_INT
 *   - DATASET_CSV / DATASET_TSV: returns scalar if 1 column, else PGBT_ARRAY of columns
 */
bool
datasetGetRow(PgBenchDataset *ds, int64 row_idx, int col_idx,
			  PgBenchValue *retval)
{
	if (ds == NULL)
		return false;

	if (row_idx < 0 || row_idx >= ds->total_rows)
	{
		pg_log_error("dataset \"%s\": row index " INT64_FORMAT " out of bounds (total_rows=" INT64_FORMAT ")",
					 ds->name, row_idx, ds->total_rows);
		return false;
	}

	switch (ds->format)
	{
		case DATASET_FVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const float *floats = (const float *) (rec + sizeof(int32));

				setVectorValue(retval, floats, ds->dim, true);
				return true;
			}

		case DATASET_BVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const uint8 *bytes = rec + sizeof(int32);
				float	   *floats = pg_malloc_array(float, ds->dim);

				for (int i = 0; i < ds->dim; i++)
					floats[i] = (float) bytes[i];

				retval->type = PGBT_VECTOR;
				retval->u.vval.dim = ds->dim;
				retval->u.vval.is_borrowed = false;
				retval->u.vval.elems = floats;
				return true;
			}

		case DATASET_IVECS:
			{
				const uint8 *rec = ds->raw_data + (size_t) row_idx * ds->record_stride;
				const int32 *ints = (const int32 *) (rec + sizeof(int32));
				PgBenchValue *elems = pg_malloc_array(PgBenchValue, ds->dim);

				for (int i = 0; i < ds->dim; i++)
				{
					int32		v;

					memcpy(&v, &ints[i], sizeof(int32));
					setIntValue(&elems[i], (int64) v);
				}

				retval->type = PGBT_ARRAY;
				retval->u.aval.nelems = ds->dim;
				retval->u.aval.elems = elems;
				return true;
			}

		case DATASET_CSV:
		case DATASET_TSV:
			return parseDelimitedRow(ds, row_idx, col_idx, retval);

		default:
			Assert(false);
			return false;
	}
}

/*
 * Advance the dataset's lock-free atomic cursor and return the next row index
 * (or PGBT_NULL when CURSOR_ONCE_GLOBAL is exhausted).
 */
bool
datasetNextId(PgBenchDataset *ds, DatasetCursorMode mode, PgBenchValue *retval)
{
	uint64		idx;

	if (ds == NULL)
		return false;

	if (ds->total_rows <= 0)
	{
		setNullValue(retval);
		return true;
	}

	idx = __atomic_fetch_add(&ds->cursor, 1, __ATOMIC_RELAXED);
	if (mode == CURSOR_ONCE_GLOBAL)
	{
		if (idx >= (uint64) ds->total_rows)
		{
			setNullValue(retval);
			return true;
		}
		setIntValue(retval, (int64) idx);
		return true;
	}
	else
	{
		setIntValue(retval, (int64) (idx % (uint64) ds->total_rows));
		return true;
	}
}

/*
 * Strip optional leading ':' from a variable name token in meta-commands.
 */
static const char *
normalizeVarArg(const char *arg)
{
	if (arg != NULL && arg[0] == ':')
		return arg + 1;
	return arg;
}

/*
 * Handler for \dataset_bind <dataset> <cursor_mode> <var1> [<var2> ...]
 */
ConnectionStateEnum
handle_cmd_dataset_bind(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *ds_name = cmd->argv[1];
	const char *mode_str = cmd->argv[2];
	PgBenchDataset *ds;
	DatasetCursorMode mode;
	PgBenchValue idx_val;
	int64		row_idx;
	int			nvars = cmd->argc - 3;

	ds = findDataset(ds_name);
	if (ds == NULL)
	{
		pg_log_error("client %d: unknown dataset \"%s\" in \\dataset_bind",
					 st->id, ds_name);
		return CSTATE_ABORTED;
	}

	if (pg_strcasecmp(mode_str, "wrap") == 0 ||
		pg_strcasecmp(mode_str, "next") == 0 ||
		pg_strcasecmp(mode_str, "round_robin") == 0 ||
		pg_strcasecmp(mode_str, "wrap_round_robin") == 0)
	{
		mode = CURSOR_WRAP_ROUND_ROBIN;
		if (!datasetNextId(ds, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (pg_strcasecmp(mode_str, "once") == 0 ||
			 pg_strcasecmp(mode_str, "once_global") == 0)
	{
		mode = CURSOR_ONCE_GLOBAL;
		if (!datasetNextId(ds, mode, &idx_val))
			return CSTATE_ABORTED;
	}
	else if (mode_str[0] == ':')
	{
		Variable   *var = lookupVariable(&st->variables, (char *) (mode_str + 1));
		int64		rval;

		if (var == NULL || !coerceToInt(&var->value, &rval))
		{
			pg_log_error("client %d: invalid row index variable \"%s\" in \\dataset_bind",
						 st->id, mode_str);
			return CSTATE_ABORTED;
		}
		setIntValue(&idx_val, rval);
	}
	else if (is_an_int(mode_str))
	{
		int64		rval = 0;

		if (!strtoint64(mode_str, true, &rval))
			return CSTATE_ABORTED;
		setIntValue(&idx_val, rval);
	}
	else
	{
		pg_log_error("client %d: invalid cursor mode \"%s\" in \\dataset_bind (expected wrap, once, or row index)",
					 st->id, mode_str);
		return CSTATE_ABORTED;
	}

	/* Exhausted ONCE_GLOBAL cursor: set all target variables to NULL */
	if (idx_val.type == PGBT_NULL)
	{
		PgBenchValue null_v;

		setNullValue(&null_v);
		for (int i = 0; i < nvars; i++)
		{
			const char *vname = normalizeVarArg(cmd->argv[3 + i]);

			if (!putVariableValue(&st->variables, "dataset_bind", (char *) vname, &null_v))
				return CSTATE_ABORTED;
		}
		return CSTATE_END_COMMAND;
	}

	row_idx = idx_val.u.ival;

	if (ds->format == DATASET_FVECS || ds->format == DATASET_BVECS ||
		ds->format == DATASET_IVECS)
	{
		PgBenchValue row_val;

		if (!datasetGetRow(ds, row_idx, -1, &row_val))
			return CSTATE_ABORTED;

		if (nvars == 1)
		{
			const char *vname = normalizeVarArg(cmd->argv[3]);
			bool		ok = putVariableValue(&st->variables, "dataset_bind",
											  (char *) vname, &row_val);

			freeValue(&row_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
		else
		{
			const char *id_var = normalizeVarArg(cmd->argv[3]);
			const char *vec_var = normalizeVarArg(cmd->argv[4]);
			bool		ok;

			if (!putVariableInt(&st->variables, "dataset_bind", (char *) id_var, row_idx))
			{
				freeValue(&row_val);
				return CSTATE_ABORTED;
			}
			ok = putVariableValue(&st->variables, "dataset_bind",
								  (char *) vec_var, &row_val);
			freeValue(&row_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
	}
	else
	{
		for (int i = 0; i < nvars; i++)
		{
			const char *vname = normalizeVarArg(cmd->argv[3 + i]);
			PgBenchValue col_val;
			bool		ok;

			if (!datasetGetRow(ds, row_idx, i, &col_val))
				return CSTATE_ABORTED;
			ok = putVariableValue(&st->variables, "dataset_bind",
								  (char *) vname, &col_val);
			freeValue(&col_val);
			if (!ok)
				return CSTATE_ABORTED;
		}
	}

	return CSTATE_END_COMMAND;
}

/*
 * Determine the number of columns targeted by table_spec for vector COPY.
 * If table_spec contains "(col1, col2, ...)", count commas + 1.
 * Otherwise, probe the table with SELECT * FROM <table> LIMIT 0.
 */
static int
detectTargetColumnCount(PGconn *con, const char *table_spec)
{
	const char *lparen = strchr(table_spec, '(');

	if (lparen != NULL)
	{
		int			cols = 1;

		for (const char *p = lparen + 1; *p != '\0' && *p != ')'; p++)
		{
			if (*p == ',')
				cols++;
		}
		return cols;
	}
	else
	{
		PQExpBufferData q;
		PGresult   *res;
		int			ncols = 2;

		initPQExpBuffer(&q);
		appendPQExpBuffer(&q, "SELECT * FROM %s LIMIT 0", table_spec);
		res = PQexec(con, q.data);
		termPQExpBuffer(&q);
		if (PQresultStatus(res) == PGRES_TUPLES_OK)
			ncols = PQnfields(res);
		PQclear(res);
		return ncols;
	}
}

/*
 * Stream the slice [start_row, end_row) of a dataset into PostgreSQL via
 * PQputCopyData() + PQputCopyEnd().
 */
bool
copyDatasetSlice(PGconn *con, PgBenchDataset *ds, const char *table_spec,
				 int client_id, int num_clients)
{
	int64		start_row;
	int64		end_row;
	PQExpBufferData copy_sql;
	PGresult   *res;

	if (ds == NULL || con == NULL)
		return false;

	if (num_clients <= 0)
		num_clients = 1;
	if (client_id < 0)
		client_id = 0;

	start_row = (ds->total_rows * (int64) client_id) / (int64) num_clients;
	end_row = (ds->total_rows * (int64) (client_id + 1)) / (int64) num_clients;

	if (start_row >= end_row)
		return true;

	initPQExpBuffer(&copy_sql);

	if (ds->format == DATASET_CSV)
	{
		char		delim = ',';
		uint64		s0 = ds->row_offsets[0];
		uint64		e0 = ds->row_offsets[1];
		bool		has_comma = false;
		bool		has_pipe = false;

		for (uint64 i = s0; i < e0 && ds->raw_data[i] != '\n'; i++)
		{
			if (ds->raw_data[i] == ',')
				has_comma = true;
			else if (ds->raw_data[i] == '|')
				has_pipe = true;
		}
		if (!has_comma && has_pipe)
			delim = '|';

		appendPQExpBuffer(&copy_sql,
						  "COPY %s FROM STDIN WITH (FORMAT csv, DELIMITER '%c')",
						  table_spec, delim);
	}
	else
	{
		appendPQExpBuffer(&copy_sql, "COPY %s FROM STDIN", table_spec);
	}

	if (ds->format == DATASET_FVECS || ds->format == DATASET_BVECS ||
		ds->format == DATASET_IVECS)
	{
		int			target_cols = detectTargetColumnCount(con, table_spec);
		PQExpBufferData chunk;

		res = PQexec(con, copy_sql.data);
		termPQExpBuffer(&copy_sql);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			pg_log_error("COPY failed for dataset \"%s\": %s",
						 ds->name, PQerrorMessage(con));
			PQclear(res);
			return false;
		}
		PQclear(res);

		initPQExpBuffer(&chunk);
		for (int64 r = start_row; r < end_row; r++)
		{
			PgBenchValue v;
			char	   *formatted;

			if (!datasetGetRow(ds, r, -1, &v))
			{
				termPQExpBuffer(&chunk);
				(void) PQputCopyEnd(con, "dataset read error");
				res = PQgetResult(con);
				PQclear(res);
				return false;
			}
			formatted = formatValue(&v);
			freeValue(&v);

			if (target_cols >= 2)
			{
				appendPQExpBuffer(&chunk, INT64_FORMAT "\t%s", r, formatted);
				for (int c = 2; c < target_cols; c++)
					appendPQExpBufferStr(&chunk, "\t\\N");
				appendPQExpBufferChar(&chunk, '\n');
			}
			else
			{
				appendPQExpBuffer(&chunk, "%s\n", formatted);
			}
			pg_free(formatted);

			if (chunk.len >= COPY_CHUNK_BYTES)
			{
				if (PQputCopyData(con, chunk.data, (int) chunk.len) != 1)
				{
					pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
								 ds->name, PQerrorMessage(con));
					termPQExpBuffer(&chunk);
					return false;
				}
				resetPQExpBuffer(&chunk);
			}
		}

		if (chunk.len > 0)
		{
			if (PQputCopyData(con, chunk.data, (int) chunk.len) != 1)
			{
				pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
							 ds->name, PQerrorMessage(con));
				termPQExpBuffer(&chunk);
				return false;
			}
		}
		termPQExpBuffer(&chunk);
	}
	else
	{
		uint64		start_byte = ds->row_offsets[start_row];
		uint64		end_byte = ds->row_offsets[end_row];
		uint64		offset = start_byte;

		res = PQexec(con, copy_sql.data);
		termPQExpBuffer(&copy_sql);
		if (PQresultStatus(res) != PGRES_COPY_IN)
		{
			pg_log_error("COPY failed for dataset \"%s\": %s",
						 ds->name, PQerrorMessage(con));
			PQclear(res);
			return false;
		}
		PQclear(res);

		while (offset < end_byte)
		{
			uint64		rem = end_byte - offset;
			int			chunk_len = (rem > COPY_CHUNK_BYTES) ? COPY_CHUNK_BYTES : (int) rem;

			if (PQputCopyData(con, (const char *) (ds->raw_data + offset), chunk_len) != 1)
			{
				pg_log_error("PQputCopyData failed for dataset \"%s\": %s",
							 ds->name, PQerrorMessage(con));
				return false;
			}
			offset += (uint64) chunk_len;
		}
	}

	if (PQputCopyEnd(con, NULL) != 1)
	{
		pg_log_error("PQputCopyEnd failed for dataset \"%s\": %s",
					 ds->name, PQerrorMessage(con));
		return false;
	}

	res = PQgetResult(con);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
	{
		pg_log_error("COPY end failed for dataset \"%s\": %s",
					 ds->name, PQerrorMessage(con));
		PQclear(res);
		return false;
	}
	PQclear(res);

	/* Drain any remaining NULL result from PQgetResult */
	while ((res = PQgetResult(con)) != NULL)
		PQclear(res);

	return true;
}

typedef struct CopyDatasetWorker
{
	THREAD_T	thread;
	PGconn	   *con;
	PgBenchDataset *ds;
	const char *table_spec;
	int			worker_id;
	int			num_workers;
	bool		ok;
} CopyDatasetWorker;

static THREAD_FUNC_RETURN_TYPE THREAD_FUNC_CC
copyDatasetWorkerRun(void *arg)
{
	CopyDatasetWorker *w = (CopyDatasetWorker *) arg;

	w->ok = copyDatasetSlice(w->con, w->ds, w->table_spec,
							 w->worker_id, w->num_workers);
	THREAD_FUNC_RETURN;
}

/*
 * Execute all registered --copy-dataset specs using up to nthreads parallel workers.
 */
void
runRegisteredCopyDatasets(PGconn *con)
{
	for (int i = 0; i < num_copy_dataset_specs; i++)
	{
		PgBenchDataset *ds = findDataset(copy_specs[i].dataset_name);
		int			num_workers = (nthreads > 1) ? nthreads : 1;

		if (ds == NULL)
			pg_fatal("--copy-dataset references unknown dataset \"%s\"",
					 copy_specs[i].dataset_name);

		if (ds->total_rows > 0 && (int64) num_workers > ds->total_rows)
			num_workers = (int) ds->total_rows;

		pg_log_info("copying dataset \"%s\" (" INT64_FORMAT " rows) into %s (%d workers)...",
					ds->name, ds->total_rows, copy_specs[i].table_spec, num_workers);

		if (num_workers <= 1)
		{
			if (!copyDatasetSlice(con, ds, copy_specs[i].table_spec, 0, 1))
				pg_fatal("failed to copy dataset \"%s\" into %s",
						 ds->name, copy_specs[i].table_spec);
		}
		else
		{
			CopyDatasetWorker *workers = pg_malloc0_array(CopyDatasetWorker, num_workers);
			bool		all_ok = true;

			workers[0].con = con;
			workers[0].ds = ds;
			workers[0].table_spec = copy_specs[i].table_spec;
			workers[0].worker_id = 0;
			workers[0].num_workers = num_workers;

			for (int w = 1; w < num_workers; w++)
			{
				workers[w].con = doConnect();
				if (workers[w].con == NULL)
					pg_fatal("could not create connection for --copy-dataset worker %d", w);
				workers[w].ds = ds;
				workers[w].table_spec = copy_specs[i].table_spec;
				workers[w].worker_id = w;
				workers[w].num_workers = num_workers;

				errno = THREAD_CREATE(&workers[w].thread, copyDatasetWorkerRun, &workers[w]);
				if (errno != 0)
					pg_fatal("could not create --copy-dataset worker thread: %m");
			}

			workers[0].ok = copyDatasetSlice(workers[0].con, ds,
											 copy_specs[i].table_spec,
											 0, num_workers);
			if (!workers[0].ok)
				all_ok = false;

			for (int w = 1; w < num_workers; w++)
			{
				THREAD_JOIN(workers[w].thread);
				PQfinish(workers[w].con);
				if (!workers[w].ok)
					all_ok = false;
			}
			pg_free(workers);

			if (!all_ok)
				pg_fatal("failed to copy dataset \"%s\" into %s",
						 ds->name, copy_specs[i].table_spec);
		}
	}
}

/*
 * Handler for \copy_dataset <dataset> <table> [(cols)]
 */
ConnectionStateEnum
handle_cmd_copy_dataset(CState *st, Command *cmd, pg_time_usec_t *now)
{
	const char *ds_name = cmd->argv[1];
	PQExpBufferData table_spec;
	PgBenchDataset *ds;
	bool		ok;

	ds = findDataset(ds_name);
	if (ds == NULL)
	{
		pg_log_error("client %d: unknown dataset \"%s\" in \\copy_dataset",
					 st->id, ds_name);
		return CSTATE_ABORTED;
	}

	initPQExpBuffer(&table_spec);
	for (int i = 2; i < cmd->argc; i++)
	{
		if (i > 2)
			appendPQExpBufferChar(&table_spec, ' ');
		appendPQExpBufferStr(&table_spec, cmd->argv[i]);
	}

	ok = copyDatasetSlice(st->con, ds, table_spec.data, st->id, nclients);
	termPQExpBuffer(&table_spec);

	if (!ok)
	{
		commandFailed(st, "copy_dataset", "bulk dataset COPY failed");
		return CSTATE_ABORTED;
	}

	return CSTATE_END_COMMAND;
}
