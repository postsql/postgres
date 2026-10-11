/*-------------------------------------------------------------------------
 *
 * pg_cascade.c
 *	  Multi-stage cascade encoding context handler (cascade), fused
 *	  single-stage Avro+Zstd handler (avro_zstd), and multi-stage dictionary
 *	  trainer for PostgreSQL.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_cascade/pg_cascade.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/encoding_context.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_encoding_type.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/regproc.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

#include <dlfcn.h>
#include <math.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_cascade_context_encode);
PG_FUNCTION_INFO_V1(pg_cascade_context_decode);
PG_FUNCTION_INFO_V1(pg_avro_zstd_context_encode);
PG_FUNCTION_INFO_V1(pg_avro_zstd_context_decode);
PG_FUNCTION_INFO_V1(pg_cascade_train_dictionary);

/* ========================================================================
 *		1. Multi-Stage Cascade Encoding Context Handler (cascade)
 * ========================================================================
 */

typedef struct CascadeState
{
	int			nstages;
	Oid			stage_oids[MAX_ENCODING_CONTEXT_DEPTH];
} CascadeState;

static int	cascade_active_depth = 0;

static Oid
resolve_cascade_stage_token(const char *token, Oid owner_ctx_id)
{
	bool		all_digits = true;

	if (*token == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("empty stage name in \"stages\" option for encoding context %u",
						owner_ctx_id)));

	for (const char *c = token; *c != '\0'; c++)
	{
		if (*c < '0' || *c > '9')
		{
			all_digits = false;
			break;
		}
	}

	if (all_digits)
	{
		unsigned long ul = strtoul(token, NULL, 10);

		return (Oid) ul;
	}
	else
	{
		List	   *qname = stringToQualifiedNameList(token, NULL);
		Oid			stage_oid = get_encoding_context_oid(qname, false);

		list_free_deep(qname);
		return stage_oid;
	}
}

static void
validate_cascade_stage_graph(Oid root_ctx_id, EncodingContext curr_ectx, int depth)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cascade encoding context %u exceeds maximum pipeline depth (%d)",
						root_ctx_id, MAX_ENCODING_CONTEXT_DEPTH)));

	for (EncodingContext walk = curr_ectx; walk != NULL; walk = walk->parent)
	{
		const char *sub_stages;

		CHECK_FOR_INTERRUPTS();
		if (walk->context_id == root_ctx_id)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("circular stage reference detected in cascade encoding context %u",
							root_ctx_id)));

		sub_stages = EncodingContextGetOption(walk, "stages");
		if (sub_stages != NULL)
		{
			char	   *raw = pstrdup(sub_stages);
			char	   *p = raw;

			pfree((void *) sub_stages);
			while (*p != '\0')
			{
				char	   *comma = strchr(p, ',');
				char	   *tok = p;
				char	   *endp;
				Oid			sub_oid;
				EncodingContext sub_ectx;

				CHECK_FOR_INTERRUPTS();
				if (comma != NULL)
					*comma = '\0';

				while (*tok != '\0' && (*tok == ' ' || *tok == '\t' || *tok == '\n' || *tok == '\r'))
					tok++;
				endp = tok + strlen(tok);
				while (endp > tok && (endp[-1] == ' ' || endp[-1] == '\t' || endp[-1] == '\n' || endp[-1] == '\r'))
					*--endp = '\0';

				sub_oid = resolve_cascade_stage_token(tok, walk->context_id);
				if (sub_oid == root_ctx_id || sub_oid == walk->context_id)
				{
					pfree(raw);
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
							 errmsg("circular stage reference detected in cascade encoding context %u",
									root_ctx_id)));
				}

				sub_ectx = GetEncodingContext(sub_oid);
				validate_cascade_stage_graph(root_ctx_id, sub_ectx, depth + 1);

				if (comma == NULL)
					break;
				p = comma + 1;
			}
			pfree(raw);
		}
	}
}

static CascadeState *
get_cascade_state(EncodingContext ectx, bool for_decode)
{
	CascadeState *state;
	const char *stages_opt;
	char	   *raw;
	char	   *p;
	int			nstages = 0;
	Oid			stage_oids[MAX_ENCODING_CONTEXT_DEPTH];

	if (!for_decode && ectx->enc_opaque != NULL)
		return (CascadeState *) ectx->enc_opaque;
	if (for_decode && ectx->dec_opaque != NULL)
		return (CascadeState *) ectx->dec_opaque;

	stages_opt = EncodingContextGetOption(ectx, "stages");
	if (stages_opt == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cascade encoding context %u requires a \"stages\" option in WITH (...)",
						ectx->context_id),
				 errhint("Specify WITH (stages = 'stage1_ctx,stage2_ctx').")));

	raw = pstrdup(stages_opt);
	pfree((void *) stages_opt);

	p = raw;
	while (*p != '\0')
	{
		char	   *comma;
		char	   *token;
		char	   *endp;
		Oid			stage_oid = InvalidOid;
		EncodingContext stage_ectx;

		CHECK_FOR_INTERRUPTS();

		comma = strchr(p, ',');
		if (comma != NULL)
			*comma = '\0';

		token = p;
		while (*token != '\0' && (*token == ' ' || *token == '\t' || *token == '\n' || *token == '\r'))
			token++;

		endp = token + strlen(token);
		while (endp > token && (endp[-1] == ' ' || endp[-1] == '\t' || endp[-1] == '\n' || endp[-1] == '\r'))
			*--endp = '\0';

		if (nstages >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("cascade encoding context %u exceeds maximum pipeline depth (%d)",
							ectx->context_id, MAX_ENCODING_CONTEXT_DEPTH)));

		stage_oid = resolve_cascade_stage_token(token, ectx->context_id);

		if (stage_oid == ectx->context_id)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("circular stage reference detected in cascade encoding context %u",
							ectx->context_id)));

		stage_ectx = GetEncodingContext(stage_oid);
		validate_cascade_stage_graph(ectx->context_id, stage_ectx, 1);

		stage_oids[nstages++] = stage_oid;

		if (comma == NULL)
			break;
		p = comma + 1;
	}

	pfree(raw);

	if (nstages == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("cascade encoding context %u requires at least one stage in \"stages\" option",
						ectx->context_id)));

	state = (CascadeState *) MemoryContextAllocZero(ectx->mcxt, sizeof(CascadeState));
	state->nstages = nstages;
	memcpy(state->stage_oids, stage_oids, sizeof(Oid) * nstages);

	if (!for_decode)
		ectx->enc_opaque = state;
	else
		ectx->dec_opaque = state;

	return state;
}

/*
 * pg_cascade_context_encode(internal, anyelement) RETURNS bytea
 */
Datum
pg_cascade_context_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		cur_val = PG_GETARG_DATUM(1);
	Oid			cur_type = get_fn_expr_argtype(fcinfo->flinfo, 1);
	CascadeState *cstate;

	check_stack_depth();

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_cascade_context_encode called with NULL EncodingContext")));

	if (cascade_active_depth >= MAX_ENCODING_CONTEXT_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cascade encoding context %u exceeds maximum pipeline depth (%d)",
						ectx->context_id, MAX_ENCODING_CONTEXT_DEPTH)));

	cstate = get_cascade_state(ectx, false);

	if (!OidIsValid(cur_type))
	{
		if (OidIsValid(ectx->target_typid))
			cur_type = ectx->target_typid;
		else if (cstate->nstages > 0)
		{
			Oid			stage0_target = GetRootEncodingContextTargetType(cstate->stage_oids[0]);

			cur_type = OidIsValid(stage0_target) ? stage0_target : BYTEAOID;
		}
		else
			cur_type = BYTEAOID;
	}

	cascade_active_depth++;
	PG_TRY();
	{
		for (int i = 0; i < cstate->nstages; i++)
		{
			EncodingContext stage_ectx;
			Datum		next_val;

			CHECK_FOR_INTERRUPTS();
			stage_ectx = GetEncodingContext(cstate->stage_oids[i]);
			next_val = EncodingContextEncode(stage_ectx, cur_val, cur_type);

			if (i > 0 && DatumGetPointer(cur_val) != DatumGetPointer(next_val))
				pfree(DatumGetPointer(cur_val));

			cur_val = next_val;
			cur_type = BYTEAOID;
		}
	}
	PG_FINALLY();
	{
		cascade_active_depth--;
	}
	PG_END_TRY();

	PG_RETURN_BYTEA_P(DatumGetByteaP(cur_val));
}

/*
 * pg_cascade_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_cascade_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	CascadeState *cstate;
	Datum		cur_val;

	check_stack_depth();

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_cascade_context_decode called with NULL EncodingContext")));

	if (cascade_active_depth >= MAX_ENCODING_CONTEXT_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("cascade encoding context %u exceeds maximum pipeline depth (%d)",
						ectx->context_id, MAX_ENCODING_CONTEXT_DEPTH)));

	cstate = get_cascade_state(ectx, true);

	if (!OidIsValid(resulttype) || resulttype == BYTEAOID)
	{
		if (OidIsValid(ectx->target_typid))
			resulttype = ectx->target_typid;
		else if (cstate->nstages > 0)
		{
			Oid			stage0_target = GetRootEncodingContextTargetType(cstate->stage_oids[0]);

			if (OidIsValid(stage0_target))
				resulttype = stage0_target;
		}
	}

	cur_val = PointerGetDatum(encoded);

	cascade_active_depth++;
	PG_TRY();
	{
		for (int i = cstate->nstages - 1; i >= 0; i--)
		{
			EncodingContext stage_ectx;
			Oid			stage_rettype;
			Datum		next_val;

			CHECK_FOR_INTERRUPTS();
			stage_ectx = GetEncodingContext(cstate->stage_oids[i]);
			stage_rettype = (i == 0) ? resulttype : BYTEAOID;

			next_val = EncodingContextDecode(stage_ectx, cur_val, stage_rettype);

			if (i < cstate->nstages - 1 &&
				DatumGetPointer(cur_val) != (Pointer) encoded &&
				DatumGetPointer(cur_val) != DatumGetPointer(next_val))
				pfree(DatumGetPointer(cur_val));

			cur_val = next_val;
		}
	}
	PG_FINALLY();
	{
		cascade_active_depth--;
	}
	PG_END_TRY();

	return cur_val;
}

/* ========================================================================
 *		2. Zstandard & LZ4 Helpers for Dictionary Training & avro_zstd
 * ========================================================================
 */

typedef struct ZSTD_CCtx_s ZSTD_CCtx;
typedef struct ZSTD_DCtx_s ZSTD_DCtx;
typedef struct ZSTD_CDict_s ZSTD_CDict;
typedef struct ZSTD_DDict_s ZSTD_DDict;

#define PG_ZSTD_CONTENTSIZE_UNKNOWN (0ULL - 1)
#define PG_ZSTD_CONTENTSIZE_ERROR   (0ULL - 2)
#define PG_ZSTD_CLEVEL_DEFAULT      3
#define PG_ZSTD_C_COMPRESSION_LEVEL 100
#define PG_ZSTD_C_CHECKSUM_FLAG     201
#define PG_ZSTD_C_DICT_ID_FLAG      202

typedef struct ZDICT_params_t
{
	int			compressionLevel;
	unsigned	notificationLevel;
	unsigned	dictID;
} ZDICT_params_t;

typedef struct ZstdLibApi
{
	bool		loaded;
	void	   *handle;
	unsigned	(*isError) (size_t code);
	const char *(*getErrorName) (size_t code);
	size_t		(*compressBound) (size_t srcSize);
	unsigned long long (*getFrameContentSize) (const void *src, size_t srcSize);
	ZSTD_CCtx  *(*createCCtx) (void);
	size_t		(*freeCCtx) (ZSTD_CCtx *cctx);
	ZSTD_DCtx  *(*createDCtx) (void);
	size_t		(*freeDCtx) (ZSTD_DCtx *dctx);
	ZSTD_CDict *(*createCDict) (const void *dictBuffer, size_t dictSize, int compressionLevel);
	size_t		(*freeCDict) (ZSTD_CDict *cdict);
	ZSTD_DDict *(*createDDict) (const void *dictBuffer, size_t dictSize);
	size_t		(*freeDDict) (ZSTD_DDict *ddict);
	size_t		(*compressCCtx) (ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
								 const void *src, size_t srcSize, int compressionLevel);
	size_t		(*compress_usingCDict) (ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
										const void *src, size_t srcSize, const ZSTD_CDict *cdict);
	size_t		(*CCtx_reset) (ZSTD_CCtx *cctx, int resetDirective);
	size_t		(*CCtx_setParameter) (ZSTD_CCtx *cctx, int param, int value);
	size_t		(*CCtx_refCDict) (ZSTD_CCtx *cctx, const ZSTD_CDict *cdict);
	size_t		(*compress2) (ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
							  const void *src, size_t srcSize);
	size_t		(*decompressDCtx) (ZSTD_DCtx *dctx, void *dst, size_t dstCapacity,
								   const void *src, size_t srcSize);
	size_t		(*decompress_usingDDict) (ZSTD_DCtx *dctx, void *dst, size_t dstCapacity,
										  const void *src, size_t srcSize, const ZSTD_DDict *ddict);
	size_t		(*trainFromBuffer) (void *dictBuffer, size_t dictBufferCapacity,
									const void *samplesBuffer, const size_t *samplesSizes,
									unsigned nbSamples);
	size_t		(*finalizeDictionary) (void *dstDictBuffer, size_t maxDictSize,
									   const void *dictContent, size_t dictContentSize,
									   const void *samplesBuffer, const size_t *samplesSizes,
									   unsigned nbSamples, ZDICT_params_t parameters);
	unsigned	(*dictIsError) (size_t errorCode);
	const char *(*dictGetErrorName) (size_t errorCode);
} ZstdLibApi;

static ZstdLibApi zstd_api = {0};

static void
ensure_zstd_loaded(void)
{
	void	   *h;

	if (zstd_api.loaded)
		return;

	h = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
	if (h == NULL)
		h = dlopen("libzstd.so", RTLD_NOW | RTLD_LOCAL);
	if (h == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not load libzstd shared library: %s", dlerror())));

	zstd_api.handle = h;
	zstd_api.isError = (unsigned (*)(size_t)) dlsym(h, "ZSTD_isError");
	zstd_api.getErrorName = (const char *(*)(size_t)) dlsym(h, "ZSTD_getErrorName");
	zstd_api.compressBound = (size_t (*)(size_t)) dlsym(h, "ZSTD_compressBound");
	zstd_api.getFrameContentSize = (unsigned long long (*)(const void *, size_t)) dlsym(h, "ZSTD_getFrameContentSize");
	zstd_api.createCCtx = (ZSTD_CCtx *(*)(void)) dlsym(h, "ZSTD_createCCtx");
	zstd_api.freeCCtx = (size_t (*)(ZSTD_CCtx *)) dlsym(h, "ZSTD_freeCCtx");
	zstd_api.createDCtx = (ZSTD_DCtx *(*)(void)) dlsym(h, "ZSTD_createDCtx");
	zstd_api.freeDCtx = (size_t (*)(ZSTD_DCtx *)) dlsym(h, "ZSTD_freeDCtx");
	zstd_api.createCDict = (ZSTD_CDict *(*)(const void *, size_t, int)) dlsym(h, "ZSTD_createCDict");
	zstd_api.freeCDict = (size_t (*)(ZSTD_CDict *)) dlsym(h, "ZSTD_freeCDict");
	zstd_api.createDDict = (ZSTD_DDict *(*)(const void *, size_t)) dlsym(h, "ZSTD_createDDict");
	zstd_api.freeDDict = (size_t (*)(ZSTD_DDict *)) dlsym(h, "ZSTD_freeDDict");
	zstd_api.compressCCtx = (size_t (*)(ZSTD_CCtx *, void *, size_t, const void *, size_t, int)) dlsym(h, "ZSTD_compressCCtx");
	zstd_api.compress_usingCDict = (size_t (*)(ZSTD_CCtx *, void *, size_t, const void *, size_t, const ZSTD_CDict *)) dlsym(h, "ZSTD_compress_usingCDict");
	zstd_api.CCtx_reset = (size_t (*)(ZSTD_CCtx *, int)) dlsym(h, "ZSTD_CCtx_reset");
	zstd_api.CCtx_setParameter = (size_t (*)(ZSTD_CCtx *, int, int)) dlsym(h, "ZSTD_CCtx_setParameter");
	zstd_api.CCtx_refCDict = (size_t (*)(ZSTD_CCtx *, const ZSTD_CDict *)) dlsym(h, "ZSTD_CCtx_refCDict");
	zstd_api.compress2 = (size_t (*)(ZSTD_CCtx *, void *, size_t, const void *, size_t)) dlsym(h, "ZSTD_compress2");
	zstd_api.decompressDCtx = (size_t (*)(ZSTD_DCtx *, void *, size_t, const void *, size_t)) dlsym(h, "ZSTD_decompressDCtx");
	zstd_api.decompress_usingDDict = (size_t (*)(ZSTD_DCtx *, void *, size_t, const void *, size_t, const ZSTD_DDict *)) dlsym(h, "ZSTD_decompress_usingDDict");
	zstd_api.trainFromBuffer = (size_t (*)(void *, size_t, const void *, const size_t *, unsigned)) dlsym(h, "ZDICT_trainFromBuffer");
	zstd_api.finalizeDictionary = dlsym(h, "ZDICT_finalizeDictionary");
	zstd_api.dictIsError = (unsigned (*)(size_t)) dlsym(h, "ZDICT_isError");
	zstd_api.dictGetErrorName = (const char *(*)(size_t)) dlsym(h, "ZDICT_getErrorName");

	if (!zstd_api.isError || !zstd_api.getErrorName || !zstd_api.compressBound ||
		!zstd_api.getFrameContentSize || !zstd_api.createCCtx || !zstd_api.freeCCtx ||
		!zstd_api.createDCtx || !zstd_api.freeDCtx || !zstd_api.createCDict ||
		!zstd_api.freeCDict || !zstd_api.createDDict || !zstd_api.freeDDict ||
		!zstd_api.compressCCtx || !zstd_api.compress_usingCDict ||
		!zstd_api.decompressDCtx || !zstd_api.decompress_usingDDict ||
		!zstd_api.trainFromBuffer)
	{
		dlclose(h);
		zstd_api.handle = NULL;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("libzstd is missing required ZSTD/ZDICT symbols")));
	}

	zstd_api.loaded = true;
}

typedef struct ZstdEncState
{
	ZSTD_CCtx  *cctx;
	ZSTD_CDict *cdict;
	int			level;
	bool		checksum;
	bool		dict_id_flag;
	MemoryContextCallback cb;
} ZstdEncState;

typedef struct ZstdDecState
{
	ZSTD_DCtx  *dctx;
	ZSTD_DDict *ddict;
	MemoryContextCallback cb;
} ZstdDecState;

static void
zstd_enc_state_cleanup(void *arg)
{
	ZstdEncState *state = (ZstdEncState *) arg;

	if (state->cdict != NULL && zstd_api.freeCDict != NULL)
	{
		zstd_api.freeCDict(state->cdict);
		state->cdict = NULL;
	}
	if (state->cctx != NULL && zstd_api.freeCCtx != NULL)
	{
		zstd_api.freeCCtx(state->cctx);
		state->cctx = NULL;
	}
}

static void
zstd_dec_state_cleanup(void *arg)
{
	ZstdDecState *state = (ZstdDecState *) arg;

	if (state->ddict != NULL && zstd_api.freeDDict != NULL)
	{
		zstd_api.freeDDict(state->ddict);
		state->ddict = NULL;
	}
	if (state->dctx != NULL && zstd_api.freeDCtx != NULL)
	{
		zstd_api.freeDCtx(state->dctx);
		state->dctx = NULL;
	}
}

static int
zstd_get_compression_level(EncodingContext ectx)
{
	const char *lvl_str = EncodingContextGetOption(ectx, "compression_level");
	int			level = PG_ZSTD_CLEVEL_DEFAULT;

	if (lvl_str == NULL)
		lvl_str = EncodingContextGetOption(ectx, "level");

	if (lvl_str != NULL)
	{
		char	   *endptr = NULL;
		long		val = strtol(lvl_str, &endptr, 10);

		if (endptr == lvl_str || *endptr != '\0' || val < -131072 || val > 22)
		{
			char	   *copy = pstrdup(lvl_str);

			pfree((void *) lvl_str);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid zstd compression_level \"%s\" for encoding context %u (must be an integer between -131072 and 22)",
							copy, ectx->context_id)));
		}
		level = (int) val;
		pfree((void *) lvl_str);
	}
	return level;
}

static bool
zstd_get_bool_option(EncodingContext ectx, const char *key, bool default_val)
{
	const char *val_str = EncodingContextGetOption(ectx, key);
	bool		res = default_val;

	if (val_str != NULL)
	{
		if (pg_strcasecmp(val_str, "true") == 0 ||
			pg_strcasecmp(val_str, "on") == 0 ||
			pg_strcasecmp(val_str, "1") == 0 ||
			pg_strcasecmp(val_str, "yes") == 0)
			res = true;
		else if (pg_strcasecmp(val_str, "false") == 0 ||
				 pg_strcasecmp(val_str, "off") == 0 ||
				 pg_strcasecmp(val_str, "0") == 0 ||
				 pg_strcasecmp(val_str, "no") == 0)
			res = false;
		else
		{
			char	   *copy = pstrdup(val_str);

			pfree((void *) val_str);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid boolean value \"%s\" for option \"%s\" in encoding context %u",
							copy, key, ectx->context_id)));
		}
		pfree((void *) val_str);
	}
	return res;
}

static bytea *
zstd_compress_bytes_with_state(ZstdEncState *state, const char *raw_data, Size raw_len)
{
	size_t		dst_cap;
	bytea	   *result;
	size_t		csize;

	dst_cap = zstd_api.compressBound(raw_len);
	result = (bytea *) palloc(VARHDRSZ + dst_cap);

	if (zstd_api.CCtx_reset != NULL &&
		zstd_api.CCtx_setParameter != NULL &&
		zstd_api.CCtx_refCDict != NULL &&
		zstd_api.compress2 != NULL)
	{
		zstd_api.CCtx_reset(state->cctx, 1 /* ZSTD_reset_session_only */);
		zstd_api.CCtx_setParameter(state->cctx, PG_ZSTD_C_COMPRESSION_LEVEL, state->level);
		zstd_api.CCtx_setParameter(state->cctx, PG_ZSTD_C_CHECKSUM_FLAG, state->checksum ? 1 : 0);
		zstd_api.CCtx_setParameter(state->cctx, PG_ZSTD_C_DICT_ID_FLAG, state->dict_id_flag ? 1 : 0);
		if (state->cdict != NULL)
			zstd_api.CCtx_refCDict(state->cctx, state->cdict);
		else
			zstd_api.CCtx_refCDict(state->cctx, NULL);

		csize = zstd_api.compress2(state->cctx,
								   VARDATA(result), dst_cap,
								   raw_data, raw_len);
	}
	else if (state->cdict != NULL)
	{
		csize = zstd_api.compress_usingCDict(state->cctx,
											 VARDATA(result), dst_cap,
											 raw_data, raw_len,
											 state->cdict);
	}
	else
	{
		csize = zstd_api.compressCCtx(state->cctx,
									  VARDATA(result), dst_cap,
									  raw_data, raw_len,
									  state->level);
	}

	if (zstd_api.isError(csize))
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("zstd compression failed: %s",
						zstd_api.getErrorName(csize))));

	SET_VARSIZE(result, VARHDRSZ + csize);
	return result;
}

static bytea *
zstd_decompress_bytes_with_state(ZstdDecState *state, const char *src_data, Size src_len)
{
	unsigned long long content_size;
	Size		out_cap;
	bytea	   *decompressed;
	size_t		dsize;

	content_size = zstd_api.getFrameContentSize(src_data, src_len);
	if (content_size == PG_ZSTD_CONTENTSIZE_ERROR)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted zstd frame header")));

	if (content_size != PG_ZSTD_CONTENTSIZE_UNKNOWN)
	{
		if (content_size > (unsigned long long) MaxAllocSize - VARHDRSZ)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("zstd decompressed size %llu exceeds MaxAllocSize",
							content_size)));
		out_cap = (Size) content_size;
	}
	else
	{
		out_cap = Max(src_len * 4, (Size) 256);
	}

	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		decompressed = (bytea *) palloc(VARHDRSZ + out_cap);
		if (state->ddict != NULL)
			dsize = zstd_api.decompress_usingDDict(state->dctx,
												   VARDATA(decompressed), out_cap,
												   src_data, src_len,
												   state->ddict);
		else
			dsize = zstd_api.decompressDCtx(state->dctx,
											VARDATA(decompressed), out_cap,
											src_data, src_len);

		if (!zstd_api.isError(dsize))
		{
			SET_VARSIZE(decompressed, VARHDRSZ + dsize);
			break;
		}

		pfree(decompressed);
		if (content_size == PG_ZSTD_CONTENTSIZE_UNKNOWN &&
			out_cap < MaxAllocSize - VARHDRSZ)
		{
			Size		next_cap = out_cap * 2;

			if (next_cap > MaxAllocSize - VARHDRSZ || next_cap <= out_cap)
				next_cap = MaxAllocSize - VARHDRSZ;
			if (next_cap > out_cap)
			{
				out_cap = next_cap;
				continue;
			}
		}

		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("zstd decompression failed: %s",
						zstd_api.getErrorName(dsize))));
	}

	return decompressed;
}

typedef struct Lz4GramEntry
{
	uint64		gram;
	uint8		len;
	uint32		count;
} Lz4GramEntry;

static int
lz4_gram_cmp_desc(const void *a, const void *b)
{
	const Lz4GramEntry *ga = (const Lz4GramEntry *) a;
	const Lz4GramEntry *gb = (const Lz4GramEntry *) b;
	uint64		sa = (uint64) ga->count * ga->len;
	uint64		sb = (uint64) gb->count * gb->len;

	if (sb > sa)
		return 1;
	if (sb < sa)
		return -1;
	if (ga->gram < gb->gram)
		return -1;
	if (ga->gram > gb->gram)
		return 1;
	return 0;
}

/*
 * pg_cascade_train_dictionary(stage_context_id oid, samples anyarray,
 *                             max_dict_size int4 DEFAULT 65536,
 *                             algorithm text DEFAULT 'zstd')
 * RETURNS bytea
 *
 * Encodes all non-NULL elements of `samples` through `stage_context_id` and
 * trains a Zstandard or LZ4 dictionary on the Stage-1 binary outputs.
 */
Datum
pg_cascade_train_dictionary(PG_FUNCTION_ARGS)
{
	Oid			stage_ctx_id = PG_GETARG_OID(0);
	ArrayType  *arr = PG_GETARG_ARRAYTYPE_P(1);
	int32		max_dict_size = PG_GETARG_INT32(2);
	text	   *algo_txt = PG_GETARG_TEXT_PP(3);
	char	   *algo_str = text_to_cstring(algo_txt);
	EncodingContext stage_ectx;
	Oid			elem_type;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	bytea	  **encoded_samples;
	int			valid_samples = 0;
	bytea	   *dict_result = NULL;

	if (max_dict_size < 256 || max_dict_size > 10485760)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("max_dict_size must be between 256 and 10485760 bytes")));

	stage_ectx = GetEncodingContext(stage_ctx_id);
	elem_type = ARR_ELEMTYPE(arr);
	get_typlenbyvalalign(elem_type, &typlen, &typbyval, &typalign);
	deconstruct_array(arr, elem_type, typlen, typbyval, typalign,
					  &elems, &nulls, &nelems);

	if (nelems <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_cascade_train_dictionary requires a non-empty array of samples")));

	encoded_samples = (bytea **) palloc0(sizeof(bytea *) * nelems);
	for (int i = 0; i < nelems; i++)
	{
		Datum		enc_datum;
		bytea	   *enc_b;

		CHECK_FOR_INTERRUPTS();
		if (nulls[i])
			continue;

		enc_datum = EncodingContextEncode(stage_ectx, elems[i], elem_type);
		enc_b = DatumGetByteaPP(enc_datum);
		if (VARSIZE_ANY_EXHDR(enc_b) > 0)
			encoded_samples[valid_samples++] = enc_b;
	}

	if (valid_samples == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_cascade_train_dictionary requires at least one non-empty encoded sample")));

	if (pg_strcasecmp(algo_str, "zstd") == 0)
	{
		StringInfoData samples_buf;
		size_t	   *sample_sizes;
		unsigned	cap_samples;
		unsigned	nb_samples = 0;
		size_t		dict_size;

		ensure_zstd_loaded();

		initStringInfo(&samples_buf);
		cap_samples = Max(valid_samples * 16, 64);
		sample_sizes = (size_t *) palloc(sizeof(size_t) * cap_samples);

		for (int i = 0; i < valid_samples; i++)
		{
			Size		sz = VARSIZE_ANY_EXHDR(encoded_samples[i]);

			CHECK_FOR_INTERRUPTS();
			appendBinaryStringInfo(&samples_buf, VARDATA_ANY(encoded_samples[i]), (int) sz);
			sample_sizes[nb_samples++] = sz;
		}

		/*
		 * ZDICT_trainFromBuffer (FastCover) requires at least ~8 samples and
		 * a minimum corpus size. Replicate small sample arrays up to at least
		 * 16 samples and 4 KB so ZDICT_trainFromBuffer can build a formatted
		 * Zstandard dictionary.
		 */
		if (nb_samples < 16 || samples_buf.len < 4096)
		{
			unsigned	orig_nb = nb_samples;
			int			orig_len = samples_buf.len;
			char	   *orig_data = (char *) palloc(orig_len);
			size_t	   *orig_sizes = (size_t *) palloc(sizeof(size_t) * orig_nb);

			memcpy(orig_data, samples_buf.data, orig_len);
			memcpy(orig_sizes, sample_sizes, sizeof(size_t) * orig_nb);

			while ((nb_samples < 16 || samples_buf.len < 4096) &&
				   nb_samples < 65536)
			{
				CHECK_FOR_INTERRUPTS();
				if (nb_samples + orig_nb > cap_samples)
				{
					cap_samples = Max(cap_samples * 2, nb_samples + orig_nb);
					sample_sizes = (size_t *) repalloc(sample_sizes, sizeof(size_t) * cap_samples);
				}
				appendBinaryStringInfo(&samples_buf, orig_data, orig_len);
				for (unsigned j = 0; j < orig_nb; j++)
					sample_sizes[nb_samples++] = orig_sizes[j];
			}

			pfree(orig_data);
			pfree(orig_sizes);
		}

		dict_result = (bytea *) palloc(VARHDRSZ + max_dict_size);
		dict_size = zstd_api.trainFromBuffer(VARDATA(dict_result),
											 (size_t) max_dict_size,
											 samples_buf.data,
											 sample_sizes,
											 nb_samples);

		if (((zstd_api.dictIsError ? zstd_api.dictIsError(dict_size) : zstd_api.isError(dict_size)) ||
			 dict_size == 0) &&
			zstd_api.finalizeDictionary != NULL && samples_buf.len >= 256)
		{
			ZDICT_params_t zparams;
			Size		content_len = Min((Size) samples_buf.len, (Size) max_dict_size / 2);

			memset(&zparams, 0, sizeof(zparams));
			zparams.compressionLevel = PG_ZSTD_CLEVEL_DEFAULT;
			if (content_len < 256)
				content_len = Min((Size) samples_buf.len, (Size) max_dict_size);
			dict_size = zstd_api.finalizeDictionary(VARDATA(dict_result),
													(size_t) max_dict_size,
													samples_buf.data,
													content_len,
													samples_buf.data,
													sample_sizes,
													nb_samples,
													zparams);
		}

		if ((zstd_api.dictIsError ? zstd_api.dictIsError(dict_size) : zstd_api.isError(dict_size)) ||
			dict_size == 0)
		{
			Size		raw_dict_len = Min((Size) samples_buf.len, (Size) max_dict_size);

			memcpy(VARDATA(dict_result), samples_buf.data, raw_dict_len);
			dict_size = raw_dict_len;
		}

		pfree(samples_buf.data);
		pfree(sample_sizes);

		SET_VARSIZE(dict_result, VARHDRSZ + dict_size);
	}
	else if (pg_strcasecmp(algo_str, "lz4") == 0)
	{
		const uint32 table_size = 16384;
		Lz4GramEntry *table;
		Lz4GramEntry *sorted;
		uint32		used = 0;
		Size		out_len = 0;
		int32		lz4_max = (max_dict_size > 65536) ? 65536 : max_dict_size;

		table = (Lz4GramEntry *) palloc0(sizeof(Lz4GramEntry) * table_size);

		for (int i = 0; i < valid_samples; i++)
		{
			const uint8 *ptr = (const uint8 *) VARDATA_ANY(encoded_samples[i]);
			Size		len = VARSIZE_ANY_EXHDR(encoded_samples[i]);
			Size		step = (len > 4096) ? (len / 2048) : 1;

			CHECK_FOR_INTERRUPTS();
			for (Size pos = 0; pos + 2 <= len; pos += step)
			{
				static const uint8 gram_lens[3] = {8, 4, 2};

				for (int gi = 0; gi < 3; gi++)
				{
					uint8		glen = gram_lens[gi];
					uint64		gval = 0;
					uint32		h;

					if (pos + glen > len)
						continue;
					memcpy(&gval, ptr + pos, glen);
					h = (uint32) ((gval * 11400714819323198485ULL) >> 50) & (table_size - 1);

					for (uint32 probe = 0; probe < 8; probe++)
					{
						uint32		idx = (h + probe) & (table_size - 1);

						if (table[idx].count == 0)
						{
							table[idx].gram = gval;
							table[idx].len = glen;
							table[idx].count = 1;
							used++;
							break;
						}
						else if (table[idx].gram == gval && table[idx].len == glen)
						{
							table[idx].count++;
							break;
						}
					}
				}
			}
		}

		sorted = (Lz4GramEntry *) palloc(sizeof(Lz4GramEntry) * Max(used, 1));
		used = 0;
		for (uint32 idx = 0; idx < table_size; idx++)
		{
			if (table[idx].count >= 2)
				sorted[used++] = table[idx];
		}
		if (used == 0)
		{
			for (uint32 idx = 0; idx < table_size; idx++)
			{
				if (table[idx].count >= 1)
					sorted[used++] = table[idx];
			}
		}

		if (used > 1)
			qsort(sorted, used, sizeof(Lz4GramEntry), lz4_gram_cmp_desc);

		dict_result = (bytea *) palloc(VARHDRSZ + lz4_max);
		for (int pass = (int) used - 1; pass >= 0; pass--)
		{
			uint8		glen = sorted[pass].len;

			if (out_len + glen > (Size) lz4_max)
				continue;
			memcpy(VARDATA(dict_result) + out_len, &sorted[pass].gram, glen);
			out_len += glen;
		}

		SET_VARSIZE(dict_result, VARHDRSZ + out_len);
		pfree(sorted);
		pfree(table);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported cascade dictionary training algorithm \"%s\"",
						algo_str),
				 errhint("Supported algorithms are \"zstd\" and \"lz4\".")));
	}

	pfree(algo_str);
	pfree(encoded_samples);
	pfree(elems);
	pfree(nulls);

	PG_RETURN_BYTEA_P(dict_result);
}

/* ========================================================================
 *					Apache Avro Schema AST & Compiler
 * ========================================================================
 */

typedef enum AvroTypeKind
{
	AVRO_NULL = 0,
	AVRO_BOOLEAN,
	AVRO_INT,
	AVRO_LONG,
	AVRO_FLOAT,
	AVRO_DOUBLE,
	AVRO_BYTES,
	AVRO_STRING,
	AVRO_RECORD,
	AVRO_ENUM,
	AVRO_ARRAY,
	AVRO_MAP,
	AVRO_UNION,
	AVRO_FIXED
} AvroTypeKind;

typedef struct AvroSchemaNode AvroSchemaNode;

typedef struct AvroFieldDef
{
	char	   *name;
	AvroSchemaNode *type;
	bool		has_default;
	Jsonb	   *default_jsonb;
} AvroFieldDef;

struct AvroSchemaNode
{
	AvroTypeKind kind;
	char	   *name;			/* for record, enum, fixed */
	int			nfields;		/* for record */
	AvroFieldDef *fields;		/* for record */
	int			nsymbols;		/* for enum */
	char	  **symbols;		/* for enum */
	AvroSchemaNode *items;		/* for array */
	AvroSchemaNode *values;		/* for map */
	int			nbranches;		/* for union */
	AvroSchemaNode **branches;	/* for union */
	int32		fixed_size;		/* for fixed */
};

typedef struct AvroNamedTypeEntry
{
	char	   *name;
	AvroSchemaNode *node;
} AvroNamedTypeEntry;

typedef struct AvroParseContext
{
	MemoryContext mcxt;
	int			num_named;
	int			cap_named;
	AvroNamedTypeEntry *named_types;
} AvroParseContext;

typedef struct AvroDecPair
{
	AvroSchemaNode *writer_schema;
	AvroSchemaNode *reader_schema;
} AvroDecPair;

static AvroSchemaNode *avro_parse_schema_jbv(JsonbValue *jbv, AvroParseContext *pctx);

static const char *
avro_type_kind_name(AvroTypeKind kind)
{
	switch (kind)
	{
		case AVRO_NULL:
			return "null";
		case AVRO_BOOLEAN:
			return "boolean";
		case AVRO_INT:
			return "int";
		case AVRO_LONG:
			return "long";
		case AVRO_FLOAT:
			return "float";
		case AVRO_DOUBLE:
			return "double";
		case AVRO_BYTES:
			return "bytes";
		case AVRO_STRING:
			return "string";
		case AVRO_RECORD:
			return "record";
		case AVRO_ENUM:
			return "enum";
		case AVRO_ARRAY:
			return "array";
		case AVRO_MAP:
			return "map";
		case AVRO_UNION:
			return "union";
		case AVRO_FIXED:
			return "fixed";
	}
	return "unknown";
}

static bool
avro_lookup_primitive(const char *tname, AvroTypeKind *out_kind)
{
	if (strcmp(tname, "null") == 0)
		*out_kind = AVRO_NULL;
	else if (strcmp(tname, "boolean") == 0)
		*out_kind = AVRO_BOOLEAN;
	else if (strcmp(tname, "int") == 0)
		*out_kind = AVRO_INT;
	else if (strcmp(tname, "long") == 0)
		*out_kind = AVRO_LONG;
	else if (strcmp(tname, "float") == 0)
		*out_kind = AVRO_FLOAT;
	else if (strcmp(tname, "double") == 0)
		*out_kind = AVRO_DOUBLE;
	else if (strcmp(tname, "bytes") == 0)
		*out_kind = AVRO_BYTES;
	else if (strcmp(tname, "string") == 0)
		*out_kind = AVRO_STRING;
	else
		return false;
	return true;
}

static AvroSchemaNode *
avro_find_named_type(AvroParseContext *pctx, const char *name)
{
	for (int i = 0; i < pctx->num_named; i++)
	{
		CHECK_FOR_INTERRUPTS();
		if (strcmp(pctx->named_types[i].name, name) == 0)
			return pctx->named_types[i].node;
	}
	return NULL;
}

static void
avro_register_named_type(AvroParseContext *pctx, const char *name, AvroSchemaNode *node)
{
	if (name == NULL || name[0] == '\0')
		return;

	if (avro_find_named_type(pctx, name) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("duplicate Avro named type \"%s\" in schema", name)));

	if (pctx->num_named >= pctx->cap_named)
	{
		int			new_cap = (pctx->cap_named == 0) ? 8 : pctx->cap_named * 2;
		AvroNamedTypeEntry *new_arr;

		new_arr = (AvroNamedTypeEntry *) MemoryContextAlloc(pctx->mcxt,
															sizeof(AvroNamedTypeEntry) * new_cap);
		if (pctx->num_named > 0)
			memcpy(new_arr, pctx->named_types, sizeof(AvroNamedTypeEntry) * pctx->num_named);
		pctx->named_types = new_arr;
		pctx->cap_named = new_cap;
	}

	pctx->named_types[pctx->num_named].name = MemoryContextStrdup(pctx->mcxt, name);
	pctx->named_types[pctx->num_named].node = node;
	pctx->num_named++;
}

static JsonbValue *
avro_get_object_field(JsonbContainer *jc, const char *key)
{
	JsonbValue	kval;

	kval.type = jbvString;
	kval.val.string.val = unconstify(char *, key);
	kval.val.string.len = (int) strlen(key);

	return findJsonbValueFromContainer(jc, JB_FOBJECT, &kval);
}

static char *
avro_jbv_to_cstring(MemoryContext mcxt, JsonbValue *jbv)
{
	char	   *res;

	if (jbv == NULL || jbv->type != jbvString)
		return NULL;

	res = (char *) MemoryContextAlloc(mcxt, jbv->val.string.len + 1);
	if (jbv->val.string.len > 0)
		memcpy(res, jbv->val.string.val, jbv->val.string.len);
	res[jbv->val.string.len] = '\0';
	return res;
}

static Jsonb *
avro_jbv_to_jsonb(MemoryContext mcxt, JsonbValue *jbv)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(mcxt);
	Jsonb	   *res;

	if (jbv->type == jbvBinary)
	{
		JsonbInState state = {0};
		JsonbIterator *it;
		JsonbValue	v;
		JsonbIteratorToken tok;

		it = JsonbIteratorInit(jbv->val.binary.data);
		while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
		{
			CHECK_FOR_INTERRUPTS();
			if (tok == WJB_BEGIN_ARRAY || tok == WJB_BEGIN_OBJECT ||
				tok == WJB_END_ARRAY || tok == WJB_END_OBJECT)
				pushJsonbValue(&state, tok, NULL);
			else
				pushJsonbValue(&state, tok, &v);
		}
		res = JsonbValueToJsonb(state.result);
	}
	else
	{
		res = JsonbValueToJsonb(jbv);
	}

	MemoryContextSwitchTo(oldcxt);
	return res;
}

static AvroSchemaNode *
avro_parse_union_array(JsonbContainer *jc, AvroParseContext *pctx)
{
	AvroSchemaNode *node;
	JsonbIterator *it;
	JsonbValue	v;
	JsonbIteratorToken tok;
	int			count = 0;
	int			idx = 0;

	check_stack_depth();

	it = JsonbIteratorInit(jc);
	tok = JsonbIteratorNext(&it, &v, true);
	if (tok != WJB_BEGIN_ARRAY)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid Avro union schema: expected JSON array")));

	count = v.val.array.nElems;
	if (count <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Avro union schema cannot be empty")));

	node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
	node->kind = AVRO_UNION;
	node->nbranches = count;
	node->branches = (AvroSchemaNode **) MemoryContextAllocZero(pctx->mcxt,
																sizeof(AvroSchemaNode *) * count);

	while ((tok = JsonbIteratorNext(&it, &v, true)) != WJB_DONE)
	{
		CHECK_FOR_INTERRUPTS();
		if (tok == WJB_END_ARRAY)
			break;
		if (tok == WJB_ELEM)
		{
			AvroSchemaNode *branch = avro_parse_schema_jbv(&v, pctx);

			if (branch->kind == AVRO_UNION)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Avro union cannot immediately contain another union")));

			for (int j = 0; j < idx; j++)
			{
				if (node->branches[j]->kind == branch->kind)
				{
					if (branch->kind == AVRO_RECORD ||
						branch->kind == AVRO_ENUM ||
						branch->kind == AVRO_FIXED)
					{
						if (branch->name && node->branches[j]->name &&
							strcmp(branch->name, node->branches[j]->name) == 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("Avro union contains duplicate named type \"%s\"",
											branch->name)));
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("Avro union contains duplicate type \"%s\"",
										avro_type_kind_name(branch->kind))));
					}
				}
			}

			node->branches[idx++] = branch;
		}
	}

	return node;
}

static AvroSchemaNode *
avro_parse_schema_jbv(JsonbValue *jbv, AvroParseContext *pctx)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	if (jbv == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Avro schema node cannot be null")));

	if (jbv->type == jbvString)
	{
		char	   *tname = avro_jbv_to_cstring(CurrentMemoryContext, jbv);
		AvroTypeKind prim_kind;
		AvroSchemaNode *node;

		if (avro_lookup_primitive(tname, &prim_kind))
		{
			node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
			node->kind = prim_kind;
			pfree(tname);
			return node;
		}

		node = avro_find_named_type(pctx, tname);
		if (node != NULL)
		{
			pfree(tname);
			return node;
		}

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown Avro type \"%s\"", tname)));
	}

	if (jbv->type == jbvBinary)
	{
		JsonbContainer *jc = jbv->val.binary.data;

		if (JsonContainerIsArray(jc))
		{
			return avro_parse_union_array(jc, pctx);
		}
		else if (JsonContainerIsObject(jc))
		{
			JsonbValue *type_jbv = avro_get_object_field(jc, "type");
			char	   *tname;
			AvroTypeKind prim_kind;
			AvroSchemaNode *node;

			if (type_jbv == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Avro schema object is missing required \"type\" field")));

			if (type_jbv->type == jbvBinary)
				return avro_parse_schema_jbv(type_jbv, pctx);

			if (type_jbv->type != jbvString)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Avro schema \"type\" property must be a string or schema")));

			tname = avro_jbv_to_cstring(CurrentMemoryContext, type_jbv);
			if (avro_lookup_primitive(tname, &prim_kind))
			{
				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = prim_kind;
				pfree(tname);
				return node;
			}

			if (strcmp(tname, "record") == 0)
			{
				JsonbValue *name_jbv = avro_get_object_field(jc, "name");
				JsonbValue *ns_jbv = avro_get_object_field(jc, "namespace");
				JsonbValue *fields_jbv = avro_get_object_field(jc, "fields");
				JsonbIterator *it;
				JsonbValue	fv;
				JsonbIteratorToken tok;
				int			fidx = 0;

				pfree(tname);
				if (name_jbv == NULL || name_jbv->type != jbvString || name_jbv->val.string.len == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro record schema requires a non-empty \"name\" string")));
				if (fields_jbv == NULL || fields_jbv->type != jbvBinary ||
					!JsonContainerIsArray(fields_jbv->val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro record schema requires a \"fields\" array")));

				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = AVRO_RECORD;
				node->name = avro_jbv_to_cstring(pctx->mcxt, name_jbv);
				avro_register_named_type(pctx, node->name, node);
				if (ns_jbv != NULL && ns_jbv->type == jbvString &&
					ns_jbv->val.string.len > 0 && strchr(node->name, '.') == NULL)
				{
					char	   *ns_str = avro_jbv_to_cstring(pctx->mcxt, ns_jbv);
					char	   *fqn = MemoryContextStrdup(pctx->mcxt, psprintf("%s.%s", ns_str, node->name));

					avro_register_named_type(pctx, fqn, node);
				}

				it = JsonbIteratorInit(fields_jbv->val.binary.data);
				tok = JsonbIteratorNext(&it, &fv, true);
				if (tok != WJB_BEGIN_ARRAY)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro record \"fields\" must be an array")));

				node->nfields = fv.val.array.nElems;
				if (node->nfields > 0)
					node->fields = (AvroFieldDef *) MemoryContextAllocZero(pctx->mcxt,
																		   sizeof(AvroFieldDef) * node->nfields);

				while ((tok = JsonbIteratorNext(&it, &fv, true)) != WJB_DONE)
				{
					CHECK_FOR_INTERRUPTS();
					if (tok == WJB_END_ARRAY)
						break;
					if (tok == WJB_ELEM)
					{
						JsonbContainer *fjc;
						JsonbValue *fname_jbv;
						JsonbValue *ftype_jbv;
						JsonbValue *fdef_jbv;

						if (fv.type != jbvBinary || !JsonContainerIsObject(fv.val.binary.data))
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("each Avro record field must be a JSON object")));

						fjc = fv.val.binary.data;
						fname_jbv = avro_get_object_field(fjc, "name");
						ftype_jbv = avro_get_object_field(fjc, "type");
						fdef_jbv = avro_get_object_field(fjc, "default");

						if (fname_jbv == NULL || fname_jbv->type != jbvString || fname_jbv->val.string.len == 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("Avro record field is missing a valid \"name\"")));
						if (ftype_jbv == NULL)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("Avro record field \"%.*s\" is missing \"type\"",
											fname_jbv->val.string.len, fname_jbv->val.string.val)));

						node->fields[fidx].name = avro_jbv_to_cstring(pctx->mcxt, fname_jbv);
						for (int k = 0; k < fidx; k++)
						{
							if (strcmp(node->fields[k].name, node->fields[fidx].name) == 0)
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
										 errmsg("duplicate field name \"%s\" in Avro record \"%s\"",
												node->fields[fidx].name, node->name)));
						}

						node->fields[fidx].type = avro_parse_schema_jbv(ftype_jbv, pctx);
						if (fdef_jbv != NULL)
						{
							node->fields[fidx].has_default = true;
							node->fields[fidx].default_jsonb = avro_jbv_to_jsonb(pctx->mcxt, fdef_jbv);
						}
						fidx++;
					}
				}
				return node;
			}
			else if (strcmp(tname, "array") == 0)
			{
				JsonbValue *items_jbv = avro_get_object_field(jc, "items");

				pfree(tname);
				if (items_jbv == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro array schema requires an \"items\" property")));

				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = AVRO_ARRAY;
				node->items = avro_parse_schema_jbv(items_jbv, pctx);
				return node;
			}
			else if (strcmp(tname, "map") == 0)
			{
				JsonbValue *values_jbv = avro_get_object_field(jc, "values");

				pfree(tname);
				if (values_jbv == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro map schema requires a \"values\" property")));

				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = AVRO_MAP;
				node->values = avro_parse_schema_jbv(values_jbv, pctx);
				return node;
			}
			else if (strcmp(tname, "enum") == 0)
			{
				JsonbValue *name_jbv = avro_get_object_field(jc, "name");
				JsonbValue *ns_jbv = avro_get_object_field(jc, "namespace");
				JsonbValue *syms_jbv = avro_get_object_field(jc, "symbols");
				JsonbIterator *it;
				JsonbValue	sv;
				JsonbIteratorToken tok;
				int			sidx = 0;

				pfree(tname);
				if (name_jbv == NULL || name_jbv->type != jbvString || name_jbv->val.string.len == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro enum schema requires a non-empty \"name\"")));
				if (syms_jbv == NULL || syms_jbv->type != jbvBinary ||
					!JsonContainerIsArray(syms_jbv->val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro enum schema requires a \"symbols\" array")));

				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = AVRO_ENUM;
				node->name = avro_jbv_to_cstring(pctx->mcxt, name_jbv);
				avro_register_named_type(pctx, node->name, node);
				if (ns_jbv != NULL && ns_jbv->type == jbvString &&
					ns_jbv->val.string.len > 0 && strchr(node->name, '.') == NULL)
				{
					char	   *ns_str = avro_jbv_to_cstring(pctx->mcxt, ns_jbv);
					char	   *fqn = MemoryContextStrdup(pctx->mcxt, psprintf("%s.%s", ns_str, node->name));

					avro_register_named_type(pctx, fqn, node);
				}

				it = JsonbIteratorInit(syms_jbv->val.binary.data);
				tok = JsonbIteratorNext(&it, &sv, true);
				if (tok != WJB_BEGIN_ARRAY || sv.val.array.nElems <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro enum \"symbols\" array must be non-empty")));

				node->nsymbols = sv.val.array.nElems;
				node->symbols = (char **) MemoryContextAllocZero(pctx->mcxt,
																 sizeof(char *) * node->nsymbols);

				while ((tok = JsonbIteratorNext(&it, &sv, true)) != WJB_DONE)
				{
					CHECK_FOR_INTERRUPTS();
					if (tok == WJB_END_ARRAY)
						break;
					if (tok == WJB_ELEM)
					{
						if (sv.type != jbvString || sv.val.string.len == 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("Avro enum symbol must be a non-empty string")));
						node->symbols[sidx] = avro_jbv_to_cstring(pctx->mcxt, &sv);
						for (int k = 0; k < sidx; k++)
						{
							if (strcmp(node->symbols[k], node->symbols[sidx]) == 0)
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
										 errmsg("duplicate symbol \"%s\" in Avro enum \"%s\"",
												node->symbols[sidx], node->name)));
						}
						sidx++;
					}
				}
				return node;
			}
			else if (strcmp(tname, "fixed") == 0)
			{
				JsonbValue *name_jbv = avro_get_object_field(jc, "name");
				JsonbValue *ns_jbv = avro_get_object_field(jc, "namespace");
				JsonbValue *size_jbv = avro_get_object_field(jc, "size");
				int32		fsize;

				pfree(tname);
				if (name_jbv == NULL || name_jbv->type != jbvString || name_jbv->val.string.len == 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro fixed schema requires a non-empty \"name\"")));
				if (size_jbv == NULL || size_jbv->type != jbvNumeric)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro fixed schema requires an integer \"size\"")));

				fsize = DatumGetInt32(DirectFunctionCall1(numeric_int4,
														  NumericGetDatum(size_jbv->val.numeric)));
				if (fsize <= 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro fixed size must be positive (got %d)", fsize)));

				node = (AvroSchemaNode *) MemoryContextAllocZero(pctx->mcxt, sizeof(AvroSchemaNode));
				node->kind = AVRO_FIXED;
				node->name = avro_jbv_to_cstring(pctx->mcxt, name_jbv);
				node->fixed_size = fsize;
				avro_register_named_type(pctx, node->name, node);
				if (ns_jbv != NULL && ns_jbv->type == jbvString &&
					ns_jbv->val.string.len > 0 && strchr(node->name, '.') == NULL)
				{
					char	   *ns_str = avro_jbv_to_cstring(pctx->mcxt, ns_jbv);
					char	   *fqn = MemoryContextStrdup(pctx->mcxt, psprintf("%s.%s", ns_str, node->name));

					avro_register_named_type(pctx, fqn, node);
				}
				return node;
			}
			else
			{
				node = avro_find_named_type(pctx, tname);
				if (node != NULL)
				{
					pfree(tname);
					return node;
				}
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("unsupported Avro schema type \"%s\"", tname)));
			}
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid Avro schema JSON structure")));
	return NULL;
}

static AvroSchemaNode *
avro_compile_schema_cstr(MemoryContext mcxt, const char *schema_cstr)
{
	Datum		jb_datum;
	Jsonb	   *jb;
	JsonbValue	root_jbv;
	AvroParseContext pctx;
	AvroSchemaNode *root_node;

	if (schema_cstr == NULL || schema_cstr[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Avro schema string cannot be empty")));

	jb_datum = DirectFunctionCall1(jsonb_in, CStringGetDatum(schema_cstr));
	jb = DatumGetJsonbP(jb_datum);

	if (JB_ROOT_IS_SCALAR(jb))
	{
		JsonbIterator *it = JsonbIteratorInit(&jb->root);
		JsonbValue	v;
		JsonbIteratorToken tok;

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		tok = JsonbIteratorNext(&it, &root_jbv, true);
		Assert(tok == WJB_ELEM);
	}
	else
	{
		root_jbv.type = jbvBinary;
		root_jbv.val.binary.data = &jb->root;
		root_jbv.val.binary.len = VARSIZE_ANY_EXHDR(jb);
	}

	MemSet(&pctx, 0, sizeof(pctx));
	pctx.mcxt = mcxt;

	root_node = avro_parse_schema_jbv(&root_jbv, &pctx);
	pfree(jb);

	return root_node;
}

static char *
avro_get_schema_string_from_ectx(EncodingContext ectx, bool is_reader)
{
	const char *opt_val = NULL;

	if (is_reader)
		opt_val = EncodingContextGetOption(ectx, "reader_schema");
	if (opt_val == NULL && !is_reader)
		opt_val = EncodingContextGetOption(ectx, "writer_schema");
	if (opt_val == NULL)
		opt_val = EncodingContextGetOption(ectx, "schema");
	if (opt_val == NULL && is_reader)
		opt_val = EncodingContextGetOption(ectx, "writer_schema");

	if (opt_val != NULL)
		return (char *) opt_val;

	if (is_reader && ectx->dectab != NULL && ectx->dectab_size > 0)
	{
		char	   *str = (char *) palloc(ectx->dectab_size + 1);

		memcpy(str, VARDATA_ANY(ectx->dectab), ectx->dectab_size);
		str[ectx->dectab_size] = '\0';
		return str;
	}

	if (ectx->enctab != NULL && ectx->enctab_size > 0)
	{
		char	   *str = (char *) palloc(ectx->enctab_size + 1);

		memcpy(str, VARDATA_ANY(ectx->enctab), ectx->enctab_size);
		str[ectx->enctab_size] = '\0';
		return str;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("encoding context %u has no Avro schema configured in OPTIONS or ENCTAB",
					ectx->context_id)));
	return NULL;
}

static inline void
avro_write_varint64(StringInfo buf, int64 val)
{
	uint64		zz = (val < 0) ? (((uint64) (~val) << 1) | 1ULL) : ((uint64) val << 1);

	while ((zz & ~0x7FULL) != 0)
	{
		appendStringInfoCharMacro(buf, (char) ((zz & 0x7FULL) | 0x80ULL));
		zz >>= 7;
	}
	appendStringInfoCharMacro(buf, (char) (zz & 0x7FULL));
}

static inline int64
avro_read_varint64(const uint8 **ptr, const uint8 *end, int max_bytes)
{
	uint64		zz = 0;
	int			shift = 0;
	int			nbytes = 0;

	while (*ptr < end)
	{
		uint8		b = *(*ptr)++;

		nbytes++;
		if (nbytes > max_bytes)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted Avro binary stream: varint exceeds %d bytes", max_bytes)));

		zz |= ((uint64) (b & 0x7FU)) << shift;
		if ((b & 0x80U) == 0)
			return (int64) ((zz >> 1) ^ (~(zz & 1ULL) + 1ULL));

		shift += 7;
	}

	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("truncated Avro binary stream while reading varint")));
	return 0;
}

static inline void
avro_write_bytes(StringInfo buf, const void *data, Size len)
{
	avro_write_varint64(buf, (int64) len);
	if (len > 0)
		appendBinaryStringInfo(buf, (const char *) data, (int) len);
}

static void avro_encode_jbv(StringInfo buf, AvroSchemaNode *schema, JsonbValue *jbv);

static bool
avro_numeric_is_exact_integer(Numeric num, bool check_int32)
{
	char	   *norm;
	bool		has_dot;
	ErrorSaveContext escontext = {T_ErrorSaveContext};

	if (numeric_is_nan(num) || numeric_is_inf(num))
		return false;

	norm = numeric_normalize(num);
	has_dot = (strchr(norm, '.') != NULL);
	pfree(norm);
	if (has_dot)
		return false;

	if (check_int32)
		(void) numeric_int4_safe(num, (Node *) &escontext);
	else
		(void) numeric_int8_safe(num, (Node *) &escontext);

	return !escontext.error_occurred;
}

static bool
avro_record_matches_json_object(AvroSchemaNode *rec_schema, JsonbContainer *jc)
{
	for (int i = 0; i < rec_schema->nfields; i++)
	{
		AvroFieldDef *f = &rec_schema->fields[i];
		JsonbValue *fval;

		CHECK_FOR_INTERRUPTS();
		fval = avro_get_object_field(jc, f->name);
		if (fval != NULL)
		{
			pfree(fval);
			continue;
		}
		if (f->has_default || f->type->kind == AVRO_NULL)
			continue;
		if (f->type->kind == AVRO_UNION)
		{
			bool		nullable = false;

			for (int b = 0; b < f->type->nbranches; b++)
			{
				if (f->type->branches[b]->kind == AVRO_NULL)
				{
					nullable = true;
					break;
				}
			}
			if (nullable)
				continue;
		}
		return false;
	}
	return true;
}

static int
avro_match_union_branch_for_jbv(AvroSchemaNode *union_schema, JsonbValue *jbv,
								JsonbValue *unwrapped_jbv)
{
	if (jbv == NULL || jbv->type == jbvNull)
	{
		for (int i = 0; i < union_schema->nbranches; i++)
		{
			if (union_schema->branches[i]->kind == AVRO_NULL)
			{
				unwrapped_jbv->type = jbvNull;
				return i;
			}
		}
		return -1;
	}

	if (jbv->type == jbvBinary && JsonContainerIsObject(jbv->val.binary.data))
	{
		JsonbContainer *jc = jbv->val.binary.data;

		if (JsonContainerSize(jc) == 1)
		{
			for (int i = 0; i < union_schema->nbranches; i++)
			{
				AvroSchemaNode *b = union_schema->branches[i];
				const char *bname = (b->kind == AVRO_RECORD || b->kind == AVRO_ENUM || b->kind == AVRO_FIXED)
					? b->name : avro_type_kind_name(b->kind);
				JsonbValue *inner = avro_get_object_field(jc, bname);

				if (inner != NULL)
				{
					*unwrapped_jbv = *inner;
					return i;
				}
			}
		}
	}

	for (int i = 0; i < union_schema->nbranches; i++)
	{
		AvroSchemaNode *b = union_schema->branches[i];

		CHECK_FOR_INTERRUPTS();
		switch (jbv->type)
		{
			case jbvBool:
				if (b->kind == AVRO_BOOLEAN)
				{
					*unwrapped_jbv = *jbv;
					return i;
				}
				break;
			case jbvNumeric:
				if (b->kind == AVRO_INT)
				{
					if (avro_numeric_is_exact_integer(jbv->val.numeric, true))
					{
						*unwrapped_jbv = *jbv;
						return i;
					}
				}
				else if (b->kind == AVRO_LONG)
				{
					if (avro_numeric_is_exact_integer(jbv->val.numeric, false))
					{
						*unwrapped_jbv = *jbv;
						return i;
					}
				}
				else if (b->kind == AVRO_FLOAT || b->kind == AVRO_DOUBLE)
				{
					*unwrapped_jbv = *jbv;
					return i;
				}
				break;
			case jbvString:
				if (b->kind == AVRO_ENUM)
				{
					for (int s = 0; s < b->nsymbols; s++)
					{
						if ((int) strlen(b->symbols[s]) == jbv->val.string.len &&
							memcmp(b->symbols[s], jbv->val.string.val, jbv->val.string.len) == 0)
						{
							*unwrapped_jbv = *jbv;
							return i;
						}
					}
				}
				else if (b->kind == AVRO_FIXED)
				{
					int			expected_len = b->fixed_size;
					int			actual_len = jbv->val.string.len;

					if (actual_len >= 2 &&
						jbv->val.string.val[0] == '\\' &&
						jbv->val.string.val[1] == 'x' &&
						(actual_len - 2) % 2 == 0)
					{
						actual_len = (actual_len - 2) / 2;
					}
					if (actual_len == expected_len)
					{
						*unwrapped_jbv = *jbv;
						return i;
					}
				}
				else if (b->kind == AVRO_STRING || b->kind == AVRO_BYTES)
				{
					*unwrapped_jbv = *jbv;
					return i;
				}
				break;
			case jbvBinary:
				if (JsonContainerIsArray(jbv->val.binary.data) && b->kind == AVRO_ARRAY)
				{
					*unwrapped_jbv = *jbv;
					return i;
				}
				if (JsonContainerIsObject(jbv->val.binary.data))
				{
					if (b->kind == AVRO_RECORD)
					{
						if (avro_record_matches_json_object(b, jbv->val.binary.data))
						{
							*unwrapped_jbv = *jbv;
							return i;
						}
					}
					else if (b->kind == AVRO_MAP)
					{
						*unwrapped_jbv = *jbv;
						return i;
					}
				}
				break;
			default:
				break;
		}
	}

	return -1;
}

static void
avro_encode_jsonb_datum(StringInfo buf, AvroSchemaNode *schema, Jsonb *jb)
{
	JsonbValue	v;

	if (JB_ROOT_IS_SCALAR(jb))
	{
		JsonbIterator *it = JsonbIteratorInit(&jb->root);
		JsonbValue	tmp;
		JsonbIteratorToken tok;

		tok = JsonbIteratorNext(&it, &tmp, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_ELEM);
	}
	else
	{
		v.type = jbvBinary;
		v.val.binary.data = &jb->root;
		v.val.binary.len = VARSIZE_ANY_EXHDR(jb);
	}

	avro_encode_jbv(buf, schema, &v);
}

static void
avro_encode_jbv(StringInfo buf, AvroSchemaNode *schema, JsonbValue *jbv)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (schema->kind)
	{
		case AVRO_NULL:
			if (jbv != NULL && jbv->type != jbvNull)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("expected null for Avro \"null\" type")));
			break;

		case AVRO_BOOLEAN:
			if (jbv == NULL || jbv->type != jbvBool)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("expected boolean for Avro \"boolean\" type")));
			appendStringInfoCharMacro(buf, jbv->val.boolean ? (char) 1 : (char) 0);
			break;

		case AVRO_INT:
			{
				int32		ival;

				if (jbv == NULL || jbv->type != jbvNumeric ||
					!avro_numeric_is_exact_integer(jbv->val.numeric, true))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected 32-bit integer numeric value for Avro \"int\" type")));
				ival = DatumGetInt32(DirectFunctionCall1(numeric_int4,
														 NumericGetDatum(jbv->val.numeric)));
				avro_write_varint64(buf, (int64) ival);
				break;
			}

		case AVRO_LONG:
			{
				int64		lval;

				if (jbv == NULL || jbv->type != jbvNumeric ||
					!avro_numeric_is_exact_integer(jbv->val.numeric, false))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected 64-bit integer numeric value for Avro \"long\" type")));
				lval = DatumGetInt64(DirectFunctionCall1(numeric_int8,
														 NumericGetDatum(jbv->val.numeric)));
				avro_write_varint64(buf, lval);
				break;
			}

		case AVRO_FLOAT:
			{
				float4		fval;

				if (jbv == NULL || jbv->type != jbvNumeric)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected numeric value for Avro \"float\" type")));
				fval = DatumGetFloat4(DirectFunctionCall1(numeric_float4,
														  NumericGetDatum(jbv->val.numeric)));
				appendBinaryStringInfo(buf, (const char *) &fval, sizeof(float4));
				break;
			}

		case AVRO_DOUBLE:
			{
				float8		dval;

				if (jbv == NULL || jbv->type != jbvNumeric)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected numeric value for Avro \"double\" type")));
				dval = DatumGetFloat8(DirectFunctionCall1(numeric_float8,
														  NumericGetDatum(jbv->val.numeric)));
				appendBinaryStringInfo(buf, (const char *) &dval, sizeof(float8));
				break;
			}

		case AVRO_STRING:
			if (jbv == NULL || jbv->type != jbvString)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("expected string value for Avro \"string\" type")));
			avro_write_bytes(buf, jbv->val.string.val, (Size) jbv->val.string.len);
			break;

		case AVRO_BYTES:
			{
				if (jbv == NULL || jbv->type != jbvString)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected string value for Avro \"bytes\" type")));
				if (jbv->val.string.len >= 2 &&
					jbv->val.string.val[0] == '\\' &&
					jbv->val.string.val[1] == 'x')
				{
					char	   *cstr = avro_jbv_to_cstring(CurrentMemoryContext, jbv);
					Datum		bdatum = DirectFunctionCall1(byteain, CStringGetDatum(cstr));
					bytea	   *bval = DatumGetByteaP(bdatum);

					avro_write_bytes(buf, VARDATA_ANY(bval), VARSIZE_ANY_EXHDR(bval));
					pfree(bval);
					pfree(cstr);
				}
				else
				{
					avro_write_bytes(buf, jbv->val.string.val, (Size) jbv->val.string.len);
				}
				break;
			}

		case AVRO_FIXED:
			{
				if (jbv == NULL || jbv->type != jbvString)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected string value for Avro \"fixed\" type \"%s\"",
									schema->name)));
				if (jbv->val.string.len >= 2 &&
					jbv->val.string.val[0] == '\\' &&
					jbv->val.string.val[1] == 'x')
				{
					char	   *cstr = avro_jbv_to_cstring(CurrentMemoryContext, jbv);
					Datum		bdatum = DirectFunctionCall1(byteain, CStringGetDatum(cstr));
					bytea	   *bval = DatumGetByteaP(bdatum);
					Size		blen = VARSIZE_ANY_EXHDR(bval);

					if (blen != (Size) schema->fixed_size)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("Avro fixed \"%s\" expects %d bytes, got %zu",
										schema->name, schema->fixed_size, blen)));
					appendBinaryStringInfo(buf, VARDATA_ANY(bval), schema->fixed_size);
					pfree(bval);
					pfree(cstr);
				}
				else
				{
					if (jbv->val.string.len != schema->fixed_size)
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("Avro fixed \"%s\" expects %d bytes, got %d",
										schema->name, schema->fixed_size, jbv->val.string.len)));
					appendBinaryStringInfo(buf, jbv->val.string.val, schema->fixed_size);
				}
				break;
			}

		case AVRO_ENUM:
			{
				int			found_idx = -1;

				if (jbv == NULL || jbv->type != jbvString)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected string symbol for Avro enum \"%s\"", schema->name)));

				for (int i = 0; i < schema->nsymbols; i++)
				{
					if ((int) strlen(schema->symbols[i]) == jbv->val.string.len &&
						memcmp(schema->symbols[i], jbv->val.string.val, jbv->val.string.len) == 0)
					{
						found_idx = i;
						break;
					}
				}
				if (found_idx < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("symbol \"%.*s\" is not a member of Avro enum \"%s\"",
									jbv->val.string.len, jbv->val.string.val, schema->name)));
				avro_write_varint64(buf, (int64) found_idx);
				break;
			}

		case AVRO_ARRAY:
			{
				JsonbIterator *it;
				JsonbValue	ev;
				JsonbIteratorToken tok;
				int			nelems;

				if (jbv == NULL || jbv->type != jbvBinary ||
					!JsonContainerIsArray(jbv->val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected JSON array for Avro \"array\" type")));

				it = JsonbIteratorInit(jbv->val.binary.data);
				tok = JsonbIteratorNext(&it, &ev, true);
				Assert(tok == WJB_BEGIN_ARRAY);
				nelems = ev.val.array.nElems;

				if (nelems > 0)
				{
					avro_write_varint64(buf, (int64) nelems);
					while ((tok = JsonbIteratorNext(&it, &ev, true)) != WJB_DONE)
					{
						CHECK_FOR_INTERRUPTS();
						if (tok == WJB_END_ARRAY)
							break;
						if (tok == WJB_ELEM)
							avro_encode_jbv(buf, schema->items, &ev);
					}
				}
				appendStringInfoCharMacro(buf, '\0');
				break;
			}

		case AVRO_MAP:
			{
				JsonbIterator *it;
				JsonbValue	kv;
				JsonbIteratorToken tok;
				int			npairs;

				if (jbv == NULL || jbv->type != jbvBinary ||
					!JsonContainerIsObject(jbv->val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected JSON object for Avro \"map\" type")));

				it = JsonbIteratorInit(jbv->val.binary.data);
				tok = JsonbIteratorNext(&it, &kv, true);
				Assert(tok == WJB_BEGIN_OBJECT);
				npairs = kv.val.object.nPairs;

				if (npairs > 0)
				{
					avro_write_varint64(buf, (int64) npairs);
					while ((tok = JsonbIteratorNext(&it, &kv, true)) != WJB_DONE)
					{
						JsonbValue	vv;

						CHECK_FOR_INTERRUPTS();
						if (tok == WJB_END_OBJECT)
							break;
						if (tok == WJB_KEY)
						{
							avro_write_bytes(buf, kv.val.string.val, (Size) kv.val.string.len);
							tok = JsonbIteratorNext(&it, &vv, true);
							if (tok != WJB_VALUE)
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
										 errmsg("corrupted JSON object while encoding Avro map")));
							avro_encode_jbv(buf, schema->values, &vv);
						}
					}
				}
				appendStringInfoCharMacro(buf, '\0');
				break;
			}

		case AVRO_UNION:
			{
				JsonbValue	unwrapped;
				int			bidx;

				bidx = avro_match_union_branch_for_jbv(schema, jbv, &unwrapped);
				if (bidx < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("JSON value does not match any branch of Avro union")));

				avro_write_varint64(buf, (int64) bidx);
				avro_encode_jbv(buf, schema->branches[bidx], &unwrapped);
				break;
			}

		case AVRO_RECORD:
			{
				JsonbContainer *jc;

				if (jbv == NULL || jbv->type != jbvBinary ||
					!JsonContainerIsObject(jbv->val.binary.data))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("expected JSON object for Avro record \"%s\"", schema->name)));

				jc = jbv->val.binary.data;
				for (int i = 0; i < schema->nfields; i++)
				{
					AvroFieldDef *f = &schema->fields[i];
					JsonbValue *fval = avro_get_object_field(jc, f->name);

					CHECK_FOR_INTERRUPTS();
					if (fval != NULL)
					{
						avro_encode_jbv(buf, f->type, fval);
						pfree(fval);
					}
					else if (f->has_default && f->default_jsonb != NULL)
					{
						avro_encode_jsonb_datum(buf, f->type, f->default_jsonb);
					}
					else if (f->type->kind == AVRO_NULL)
					{
						/* 0 bytes */
					}
					else if (f->type->kind == AVRO_UNION)
					{
						JsonbValue	null_jbv;
						JsonbValue	unwrapped;
						int			bidx;

						null_jbv.type = jbvNull;
						bidx = avro_match_union_branch_for_jbv(f->type, &null_jbv, &unwrapped);
						if (bidx < 0)
							ereport(ERROR,
									(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
									 errmsg("missing required field \"%s\" in Avro record \"%s\"",
											f->name, schema->name)));
						avro_write_varint64(buf, (int64) bidx);
					}
					else
					{
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("missing required field \"%s\" in Avro record \"%s\"",
										f->name, schema->name)));
					}
				}
				break;
			}
	}
}

static bool
avro_sql_type_matches_branch(Oid valtype, AvroSchemaNode *b)
{
	switch (b->kind)
	{
		case AVRO_BOOLEAN:
			return (valtype == BOOLOID);
		case AVRO_INT:
			return (valtype == INT2OID || valtype == INT4OID);
		case AVRO_LONG:
			return (valtype == INT8OID || valtype == INT4OID || valtype == INT2OID);
		case AVRO_FLOAT:
			return (valtype == FLOAT4OID);
		case AVRO_DOUBLE:
			return (valtype == FLOAT8OID || valtype == FLOAT4OID);
		case AVRO_STRING:
		case AVRO_ENUM:
			return (valtype == TEXTOID || valtype == VARCHAROID || valtype == BPCHAROID);
		case AVRO_BYTES:
		case AVRO_FIXED:
			return (valtype == BYTEAOID);
		case AVRO_ARRAY:
			return OidIsValid(get_element_type(valtype));
		case AVRO_RECORD:
			return (get_typtype(valtype) == TYPTYPE_COMPOSITE || valtype == RECORDOID);
		default:
			return false;
	}
}

static void
avro_encode_sql_datum(StringInfo buf, AvroSchemaNode *schema,
					  Datum val, bool isnull, Oid valtype)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	if (!isnull && valtype == JSONBOID)
	{
		Jsonb	   *jb = DatumGetJsonbP(val);

		avro_encode_jsonb_datum(buf, schema, jb);
		return;
	}
	if (!isnull && valtype == JSONOID)
	{
		text	   *jtxt = DatumGetTextPP(val);
		char	   *jcstr = text_to_cstring(jtxt);
		Datum		jbd = DirectFunctionCall1(jsonb_in, CStringGetDatum(jcstr));
		Jsonb	   *jb = DatumGetJsonbP(jbd);

		avro_encode_jsonb_datum(buf, schema, jb);
		pfree(jb);
		pfree(jcstr);
		return;
	}

	if (schema->kind == AVRO_UNION)
	{
		if (isnull)
		{
			for (int i = 0; i < schema->nbranches; i++)
			{
				if (schema->branches[i]->kind == AVRO_NULL)
				{
					avro_write_varint64(buf, (int64) i);
					return;
				}
			}
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("NULL SQL value is not allowed by Avro union without \"null\" branch")));
		}
		else
		{
			for (int i = 0; i < schema->nbranches; i++)
			{
				if (schema->branches[i]->kind != AVRO_NULL &&
					avro_sql_type_matches_branch(valtype, schema->branches[i]))
				{
					avro_write_varint64(buf, (int64) i);
					avro_encode_sql_datum(buf, schema->branches[i], val, false, valtype);
					return;
				}
			}
			for (int i = 0; i < schema->nbranches; i++)
			{
				if (schema->branches[i]->kind != AVRO_NULL)
				{
					avro_write_varint64(buf, (int64) i);
					avro_encode_sql_datum(buf, schema->branches[i], val, false, valtype);
					return;
				}
			}
		}
	}

	if (isnull)
	{
		if (schema->kind == AVRO_NULL)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("cannot encode NULL into non-nullable Avro type \"%s\"",
						avro_type_kind_name(schema->kind))));
	}

	switch (schema->kind)
	{
		case AVRO_NULL:
			break;

		case AVRO_BOOLEAN:
			if (valtype != BOOLOID)
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("Avro boolean expects SQL boolean, got %s", format_type_be(valtype))));
			appendStringInfoCharMacro(buf, DatumGetBool(val) ? (char) 1 : (char) 0);
			break;

		case AVRO_INT:
			if (valtype == INT2OID)
				avro_write_varint64(buf, (int64) DatumGetInt16(val));
			else if (valtype == INT4OID)
				avro_write_varint64(buf, (int64) DatumGetInt32(val));
			else if (valtype == INT8OID)
				avro_write_varint64(buf, DatumGetInt64(val));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("Avro int expects integer type, got %s", format_type_be(valtype))));
			break;

		case AVRO_LONG:
			if (valtype == INT2OID)
				avro_write_varint64(buf, (int64) DatumGetInt16(val));
			else if (valtype == INT4OID)
				avro_write_varint64(buf, (int64) DatumGetInt32(val));
			else if (valtype == INT8OID)
				avro_write_varint64(buf, DatumGetInt64(val));
			else
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("Avro long expects integer type, got %s", format_type_be(valtype))));
			break;

		case AVRO_FLOAT:
			{
				float4		fval;

				if (valtype == FLOAT4OID)
					fval = DatumGetFloat4(val);
				else if (valtype == FLOAT8OID)
					fval = (float4) DatumGetFloat8(val);
				else if (valtype == INT4OID)
					fval = (float4) DatumGetInt32(val);
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("Avro float expects float4, got %s", format_type_be(valtype))));
				appendBinaryStringInfo(buf, (const char *) &fval, sizeof(float4));
				break;
			}

		case AVRO_DOUBLE:
			{
				float8		dval;

				if (valtype == FLOAT8OID)
					dval = DatumGetFloat8(val);
				else if (valtype == FLOAT4OID)
					dval = (float8) DatumGetFloat4(val);
				else if (valtype == INT4OID)
					dval = (float8) DatumGetInt32(val);
				else if (valtype == INT8OID)
					dval = (float8) DatumGetInt64(val);
				else
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("Avro double expects float8, got %s", format_type_be(valtype))));
				appendBinaryStringInfo(buf, (const char *) &dval, sizeof(float8));
				break;
			}

		case AVRO_STRING:
		case AVRO_BYTES:
			{
				struct varlena *detoasted = pg_detoast_datum_packed(DatumGetPointer(val));

				avro_write_bytes(buf, VARDATA_ANY(detoasted), VARSIZE_ANY_EXHDR(detoasted));
				if ((Pointer) detoasted != DatumGetPointer(val))
					pfree(detoasted);
				break;
			}

		case AVRO_FIXED:
			{
				struct varlena *detoasted = pg_detoast_datum_packed(DatumGetPointer(val));
				Size		blen = VARSIZE_ANY_EXHDR(detoasted);

				if (blen != (Size) schema->fixed_size)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("Avro fixed \"%s\" expects %d bytes, got %zu",
									schema->name, schema->fixed_size, blen)));
				appendBinaryStringInfo(buf, VARDATA_ANY(detoasted), schema->fixed_size);
				if ((Pointer) detoasted != DatumGetPointer(val))
					pfree(detoasted);
				break;
			}

		case AVRO_ENUM:
			{
				text	   *txt = DatumGetTextPP(val);
				int			tlen = (int) VARSIZE_ANY_EXHDR(txt);
				const char *tptr = VARDATA_ANY(txt);
				int			found_idx = -1;

				for (int i = 0; i < schema->nsymbols; i++)
				{
					if ((int) strlen(schema->symbols[i]) == tlen &&
						memcmp(schema->symbols[i], tptr, tlen) == 0)
					{
						found_idx = i;
						break;
					}
				}
				if (found_idx < 0)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("symbol \"%.*s\" is not in Avro enum \"%s\"",
									tlen, tptr, schema->name)));
				avro_write_varint64(buf, (int64) found_idx);
				break;
			}

		case AVRO_ARRAY:
			{
				Oid			elem_type = get_element_type(valtype);
				ArrayType  *arr;
				int16		elmlen;
				bool		elmbyval;
				char		elmalign;
				Datum	   *elems;
				bool	   *nulls;
				int			nelems;

				if (!OidIsValid(elem_type))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("Avro array expects SQL array type, got %s",
									format_type_be(valtype))));

				arr = DatumGetArrayTypeP(val);
				get_typlenbyvalalign(elem_type, &elmlen, &elmbyval, &elmalign);
				deconstruct_array(arr, elem_type, elmlen, elmbyval, elmalign,
								  &elems, &nulls, &nelems);

				if (nelems > 0)
				{
					avro_write_varint64(buf, (int64) nelems);
					for (int i = 0; i < nelems; i++)
					{
						CHECK_FOR_INTERRUPTS();
						avro_encode_sql_datum(buf, schema->items, elems[i], nulls[i], elem_type);
					}
				}
				appendStringInfoCharMacro(buf, '\0');
				pfree(elems);
				pfree(nulls);
				break;
			}

		case AVRO_RECORD:
			{
				HeapTupleHeader td = DatumGetHeapTupleHeader(val);
				Oid			tupType = HeapTupleHeaderGetTypeId(td);
				int32		tupTypmod = HeapTupleHeaderGetTypMod(td);
				TupleDesc	tupdesc = lookup_rowtype_tupdesc(tupType, tupTypmod);
				HeapTupleData tmptup;
				Datum	   *vals = (Datum *) palloc(sizeof(Datum) * tupdesc->natts);
				bool	   *nulls = (bool *) palloc(sizeof(bool) * tupdesc->natts);

				tmptup.t_len = HeapTupleHeaderGetDatumLength(td);
				tmptup.t_data = td;
				heap_deform_tuple(&tmptup, tupdesc, vals, nulls);

				for (int i = 0; i < schema->nfields; i++)
				{
					AvroFieldDef *f = &schema->fields[i];
					int			att_idx = -1;

					CHECK_FOR_INTERRUPTS();
					for (int a = 0; a < tupdesc->natts; a++)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, a);

						if (!attr->attisdropped && strcmp(NameStr(attr->attname), f->name) == 0)
						{
							att_idx = a;
							break;
						}
					}

					if (att_idx >= 0)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, att_idx);

						avro_encode_sql_datum(buf, f->type, vals[att_idx], nulls[att_idx], attr->atttypid);
					}
					else if (f->has_default && f->default_jsonb != NULL)
					{
						avro_encode_jsonb_datum(buf, f->type, f->default_jsonb);
					}
					else
					{
						ReleaseTupleDesc(tupdesc);
						pfree(vals);
						pfree(nulls);
						ereport(ERROR,
								(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								 errmsg("missing composite attribute for Avro field \"%s\"", f->name)));
					}
				}

				ReleaseTupleDesc(tupdesc);
				pfree(vals);
				pfree(nulls);
				break;
			}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Avro type \"%s\" requires jsonb/json target type",
							avro_type_kind_name(schema->kind))));
	}
}

static void avro_skip_binary(AvroSchemaNode *w_schema, const uint8 **ptr, const uint8 *end);

static void
avro_emit_jsonb_value(JsonbInState *state, JsonbValue *v, JsonbIteratorToken val_tok)
{
	if (state->parseState == NULL)
	{
		pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);
		pushJsonbValue(state, WJB_ELEM, v);
		return;
	}
	pushJsonbValue(state, val_tok, v);
}

static void
avro_emit_default_jsonb(Jsonb *def_jb, JsonbInState *state, JsonbIteratorToken outer_tok)
{
	JsonbIterator *it = JsonbIteratorInit(&def_jb->root);
	JsonbValue	v;
	JsonbIteratorToken tok;

	if (JB_ROOT_IS_SCALAR(def_jb))
	{
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_ELEM);
		avro_emit_jsonb_value(state, &v, outer_tok);
		return;
	}

	while ((tok = JsonbIteratorNext(&it, &v, false)) != WJB_DONE)
	{
		CHECK_FOR_INTERRUPTS();
		if (tok == WJB_BEGIN_ARRAY || tok == WJB_BEGIN_OBJECT ||
			tok == WJB_END_ARRAY || tok == WJB_END_OBJECT)
			pushJsonbValue(state, tok, NULL);
		else
			pushJsonbValue(state, tok, &v);
	}
}

static void
avro_skip_binary(AvroSchemaNode *w_schema, const uint8 **ptr, const uint8 *end)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	switch (w_schema->kind)
	{
		case AVRO_NULL:
			break;
		case AVRO_BOOLEAN:
			if (*ptr >= end)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("truncated Avro boolean")));
			(*ptr)++;
			break;
		case AVRO_INT:
		case AVRO_ENUM:
			(void) avro_read_varint64(ptr, end, 5);
			break;
		case AVRO_LONG:
			(void) avro_read_varint64(ptr, end, 10);
			break;
		case AVRO_FLOAT:
			if (end - *ptr < 4)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("truncated Avro float")));
			*ptr += 4;
			break;
		case AVRO_DOUBLE:
			if (end - *ptr < 8)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("truncated Avro double")));
			*ptr += 8;
			break;
		case AVRO_BYTES:
		case AVRO_STRING:
			{
				int64		len = avro_read_varint64(ptr, end, 10);

				if (len < 0 || end - *ptr < len)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated or negative Avro bytes/string length %lld",
									(long long) len)));
				*ptr += len;
				break;
			}
		case AVRO_FIXED:
			if (end - *ptr < w_schema->fixed_size)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("truncated Avro fixed value")));
			*ptr += w_schema->fixed_size;
			break;
		case AVRO_ARRAY:
			{
				for (;;)
				{
					int64		cnt = avro_read_varint64(ptr, end, 10);

					CHECK_FOR_INTERRUPTS();
					if (cnt == 0)
						break;
					if (cnt < 0)
					{
						cnt = -cnt;
						(void) avro_read_varint64(ptr, end, 10);
					}
					for (int64 i = 0; i < cnt; i++)
						avro_skip_binary(w_schema->items, ptr, end);
				}
				break;
			}
		case AVRO_MAP:
			{
				for (;;)
				{
					int64		cnt = avro_read_varint64(ptr, end, 10);

					CHECK_FOR_INTERRUPTS();
					if (cnt == 0)
						break;
					if (cnt < 0)
					{
						cnt = -cnt;
						(void) avro_read_varint64(ptr, end, 10);
					}
					for (int64 i = 0; i < cnt; i++)
					{
						int64		klen = avro_read_varint64(ptr, end, 10);

						if (klen < 0 || end - *ptr < klen)
							ereport(ERROR,
									(errcode(ERRCODE_DATA_CORRUPTED),
									 errmsg("truncated Avro map key")));
						*ptr += klen;
						avro_skip_binary(w_schema->values, ptr, end);
					}
				}
				break;
			}
		case AVRO_UNION:
			{
				int64		bidx = avro_read_varint64(ptr, end, 5);

				if (bidx < 0 || bidx >= w_schema->nbranches)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted Avro union branch index %lld", (long long) bidx)));
				avro_skip_binary(w_schema->branches[bidx], ptr, end);
				break;
			}
		case AVRO_RECORD:
			for (int i = 0; i < w_schema->nfields; i++)
				avro_skip_binary(w_schema->fields[i].type, ptr, end);
			break;
	}
}

static bool
avro_schemas_compatible(AvroSchemaNode *w, AvroSchemaNode *r)
{
	if (w->kind == r->kind)
	{
		if (w->kind == AVRO_RECORD || w->kind == AVRO_ENUM || w->kind == AVRO_FIXED)
		{
			if (w->name && r->name && strcmp(w->name, r->name) != 0)
				return false;
		}
		return true;
	}

	if (w->kind == AVRO_INT &&
		(r->kind == AVRO_LONG || r->kind == AVRO_FLOAT || r->kind == AVRO_DOUBLE))
		return true;
	if (w->kind == AVRO_LONG &&
		(r->kind == AVRO_FLOAT || r->kind == AVRO_DOUBLE))
		return true;
	if (w->kind == AVRO_FLOAT && r->kind == AVRO_DOUBLE)
		return true;
	if ((w->kind == AVRO_STRING && r->kind == AVRO_BYTES) ||
		(w->kind == AVRO_BYTES && r->kind == AVRO_STRING))
		return true;

	return false;
}

static void
avro_decode_with_token(AvroSchemaNode *w_schema, AvroSchemaNode *r_schema,
					   const uint8 **ptr, const uint8 *end,
					   JsonbInState *state, JsonbIteratorToken tok_kind)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	if (w_schema->kind == AVRO_UNION)
	{
		int64		bidx = avro_read_varint64(ptr, end, 5);

		if (bidx < 0 || bidx >= w_schema->nbranches)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted Avro union index %lld (nbranches=%d)",
							(long long) bidx, w_schema->nbranches)));
		avro_decode_with_token(w_schema->branches[bidx], r_schema, ptr, end, state, tok_kind);
		return;
	}

	if (r_schema->kind == AVRO_UNION)
	{
		for (int i = 0; i < r_schema->nbranches; i++)
		{
			if (avro_schemas_compatible(w_schema, r_schema->branches[i]))
			{
				avro_decode_with_token(w_schema, r_schema->branches[i], ptr, end, state, tok_kind);
				return;
			}
		}
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Avro writer type \"%s\" is incompatible with all branches of reader union",
						avro_type_kind_name(w_schema->kind))));
	}

	if (!avro_schemas_compatible(w_schema, r_schema))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Avro writer type \"%s\" cannot be promoted to reader type \"%s\"",
						avro_type_kind_name(w_schema->kind),
						avro_type_kind_name(r_schema->kind))));

	switch (r_schema->kind)
	{
		case AVRO_NULL:
			{
				JsonbValue	v;

				v.type = jbvNull;
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_BOOLEAN:
			{
				JsonbValue	v;
				uint8		b;

				if (*ptr >= end)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated Avro boolean")));
				b = *(*ptr)++;
				if (b > 1)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted Avro boolean byte 0x%02x", b)));
				v.type = jbvBool;
				v.val.boolean = (b == 1);
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_INT:
			{
				int64		iv = avro_read_varint64(ptr, end, 5);
				JsonbValue	v;

				if (iv < PG_INT32_MIN || iv > PG_INT32_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Avro int value %lld out of 32-bit range", (long long) iv)));
				v.type = jbvNumeric;
				v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int4_numeric, Int32GetDatum((int32) iv)));
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_LONG:
			{
				int64		lv = avro_read_varint64(ptr, end, (w_schema->kind == AVRO_INT) ? 5 : 10);
				JsonbValue	v;

				v.type = jbvNumeric;
				v.val.numeric = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(lv)));
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_FLOAT:
			{
				float4		fv;
				JsonbValue	v;

				if (w_schema->kind == AVRO_INT || w_schema->kind == AVRO_LONG)
				{
					int64		lv = avro_read_varint64(ptr, end, 10);

					fv = (float4) lv;
				}
				else
				{
					if (end - *ptr < (ptrdiff_t) sizeof(float4))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro float")));
					memcpy(&fv, *ptr, sizeof(float4));
					*ptr += sizeof(float4);
				}
				v.type = jbvNumeric;
				v.val.numeric = DatumGetNumeric(DirectFunctionCall1(float4_numeric, Float4GetDatum(fv)));
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_DOUBLE:
			{
				float8		dv;
				JsonbValue	v;

				if (w_schema->kind == AVRO_INT || w_schema->kind == AVRO_LONG)
				{
					int64		lv = avro_read_varint64(ptr, end, 10);

					dv = (float8) lv;
				}
				else if (w_schema->kind == AVRO_FLOAT)
				{
					float4		fv;

					if (end - *ptr < (ptrdiff_t) sizeof(float4))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro float")));
					memcpy(&fv, *ptr, sizeof(float4));
					*ptr += sizeof(float4);
					dv = (float8) fv;
				}
				else
				{
					if (end - *ptr < (ptrdiff_t) sizeof(float8))
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro double")));
					memcpy(&dv, *ptr, sizeof(float8));
					*ptr += sizeof(float8);
				}
				v.type = jbvNumeric;
				v.val.numeric = DatumGetNumeric(DirectFunctionCall1(float8_numeric, Float8GetDatum(dv)));
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_STRING:
			{
				int64		slen = avro_read_varint64(ptr, end, 10);
				JsonbValue	v;

				if (slen < 0 || end - *ptr < slen)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted or truncated Avro string length %lld",
									(long long) slen)));
				v.type = jbvString;
				v.val.string.val = (char *) palloc(slen + 1);
				if (slen > 0)
					memcpy(v.val.string.val, *ptr, slen);
				v.val.string.val[slen] = '\0';
				v.val.string.len = (int) slen;
				*ptr += slen;
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_BYTES:
			{
				int64		slen = avro_read_varint64(ptr, end, 10);
				JsonbValue	v;
				bool		need_hex = false;

				if (slen < 0 || end - *ptr < slen)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted or truncated Avro bytes length %lld",
									(long long) slen)));
				if (slen >= 2 && (*ptr)[0] == '\\' && (*ptr)[1] == 'x')
					need_hex = true;
				for (int64 i = 0; i < slen && !need_hex; i++)
				{
					if ((*ptr)[i] < 0x20 || (*ptr)[i] > 0x7E)
						need_hex = true;
				}

				v.type = jbvString;
				if (need_hex)
				{
					static const char hexdigits[] = "0123456789abcdef";
					char	   *hexbuf = (char *) palloc(2 + slen * 2 + 1);

					hexbuf[0] = '\\';
					hexbuf[1] = 'x';
					for (int64 i = 0; i < slen; i++)
					{
						uint8		b = (*ptr)[i];

						hexbuf[2 + i * 2] = hexdigits[(b >> 4) & 0x0F];
						hexbuf[2 + i * 2 + 1] = hexdigits[b & 0x0F];
					}
					hexbuf[2 + slen * 2] = '\0';
					v.val.string.val = hexbuf;
					v.val.string.len = (int) (2 + slen * 2);
				}
				else
				{
					v.val.string.val = (char *) palloc(slen + 1);
					if (slen > 0)
						memcpy(v.val.string.val, *ptr, slen);
					v.val.string.val[slen] = '\0';
					v.val.string.len = (int) slen;
				}
				*ptr += slen;
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_FIXED:
			{
				JsonbValue	v;
				int			fsize = r_schema->fixed_size;
				bool		need_hex = false;

				if (w_schema->fixed_size != r_schema->fixed_size)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("Avro fixed size mismatch (%d vs %d)",
									w_schema->fixed_size, r_schema->fixed_size)));
				if (end - *ptr < fsize)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated Avro fixed bytes")));
				if (fsize >= 2 && (*ptr)[0] == '\\' && (*ptr)[1] == 'x')
					need_hex = true;
				for (int i = 0; i < fsize && !need_hex; i++)
				{
					if ((*ptr)[i] < 0x20 || (*ptr)[i] > 0x7E)
						need_hex = true;
				}

				v.type = jbvString;
				if (need_hex)
				{
					static const char hexdigits[] = "0123456789abcdef";
					char	   *hexbuf = (char *) palloc(2 + fsize * 2 + 1);

					hexbuf[0] = '\\';
					hexbuf[1] = 'x';
					for (int i = 0; i < fsize; i++)
					{
						uint8		b = (*ptr)[i];

						hexbuf[2 + i * 2] = hexdigits[(b >> 4) & 0x0F];
						hexbuf[2 + i * 2 + 1] = hexdigits[b & 0x0F];
					}
					hexbuf[2 + fsize * 2] = '\0';
					v.val.string.val = hexbuf;
					v.val.string.len = 2 + fsize * 2;
				}
				else
				{
					v.val.string.val = (char *) palloc(fsize + 1);
					memcpy(v.val.string.val, *ptr, fsize);
					v.val.string.val[fsize] = '\0';
					v.val.string.len = fsize;
				}
				*ptr += fsize;
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_ENUM:
			{
				int64		sidx = avro_read_varint64(ptr, end, 5);
				const char *sym_name;
				JsonbValue	v;

				if (sidx < 0 || sidx >= w_schema->nsymbols)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted Avro enum index %lld", (long long) sidx)));
				sym_name = w_schema->symbols[sidx];

				if (r_schema != w_schema)
				{
					bool		found = false;

					for (int i = 0; i < r_schema->nsymbols; i++)
					{
						if (strcmp(r_schema->symbols[i], sym_name) == 0)
						{
							found = true;
							break;
						}
					}
					if (!found)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("Avro writer enum symbol \"%s\" not present in reader enum \"%s\"",
										sym_name, r_schema->name)));
				}

				v.type = jbvString;
				v.val.string.val = unconstify(char *, sym_name);
				v.val.string.len = (int) strlen(sym_name);
				avro_emit_jsonb_value(state, &v, tok_kind);
				break;
			}

		case AVRO_ARRAY:
			{
				pushJsonbValue(state, WJB_BEGIN_ARRAY, NULL);
				for (;;)
				{
					int64		cnt = avro_read_varint64(ptr, end, 10);

					CHECK_FOR_INTERRUPTS();
					if (cnt == 0)
						break;
					if (cnt < 0)
					{
						cnt = -cnt;
						(void) avro_read_varint64(ptr, end, 10);
					}
					for (int64 i = 0; i < cnt; i++)
					{
						CHECK_FOR_INTERRUPTS();
						avro_decode_with_token(w_schema->items, r_schema->items,
											   ptr, end, state, WJB_ELEM);
					}
				}
				pushJsonbValue(state, WJB_END_ARRAY, NULL);
				break;
			}

		case AVRO_MAP:
			{
				pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
				for (;;)
				{
					int64		cnt = avro_read_varint64(ptr, end, 10);

					CHECK_FOR_INTERRUPTS();
					if (cnt == 0)
						break;
					if (cnt < 0)
					{
						cnt = -cnt;
						(void) avro_read_varint64(ptr, end, 10);
					}
					for (int64 i = 0; i < cnt; i++)
					{
						int64		klen = avro_read_varint64(ptr, end, 10);
						JsonbValue	kval;

						CHECK_FOR_INTERRUPTS();
						if (klen < 0 || end - *ptr < klen)
							ereport(ERROR,
									(errcode(ERRCODE_DATA_CORRUPTED),
									 errmsg("truncated Avro map key")));
						kval.type = jbvString;
						kval.val.string.val = (char *) palloc(klen + 1);
						if (klen > 0)
							memcpy(kval.val.string.val, *ptr, klen);
						kval.val.string.val[klen] = '\0';
						kval.val.string.len = (int) klen;
						*ptr += klen;

						pushJsonbValue(state, WJB_KEY, &kval);
						avro_decode_with_token(w_schema->values, r_schema->values,
											   ptr, end, state, WJB_VALUE);
					}
				}
				pushJsonbValue(state, WJB_END_OBJECT, NULL);
				break;
			}

		case AVRO_RECORD:
			{
				if (w_schema == r_schema)
				{
					pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
					for (int i = 0; i < r_schema->nfields; i++)
					{
						AvroFieldDef *f = &r_schema->fields[i];
						JsonbValue	kval;

						CHECK_FOR_INTERRUPTS();
						kval.type = jbvString;
						kval.val.string.val = f->name;
						kval.val.string.len = (int) strlen(f->name);
						pushJsonbValue(state, WJB_KEY, &kval);
						avro_decode_with_token(f->type, f->type, ptr, end, state, WJB_VALUE);
					}
					pushJsonbValue(state, WJB_END_OBJECT, NULL);
				}
				else
				{
					JsonbValue *decoded_r = NULL;
					bool	   *has_decoded_r = NULL;

					if (r_schema->nfields > 0)
					{
						decoded_r = (JsonbValue *) palloc0(sizeof(JsonbValue) * r_schema->nfields);
						has_decoded_r = (bool *) palloc0(sizeof(bool) * r_schema->nfields);
					}

					for (int wi = 0; wi < w_schema->nfields; wi++)
					{
						AvroFieldDef *wf = &w_schema->fields[wi];
						int			r_idx = -1;

						CHECK_FOR_INTERRUPTS();
						for (int ri = 0; ri < r_schema->nfields; ri++)
						{
							if (strcmp(r_schema->fields[ri].name, wf->name) == 0)
							{
								r_idx = ri;
								break;
							}
						}

						if (r_idx >= 0)
						{
							AvroFieldDef *rf = &r_schema->fields[r_idx];
							JsonbInState fstate = {0};

							pushJsonbValue(&fstate, WJB_BEGIN_ARRAY, NULL);
							avro_decode_with_token(wf->type, rf->type, ptr, end, &fstate, WJB_ELEM);
							pushJsonbValue(&fstate, WJB_END_ARRAY, NULL);
							Assert(fstate.result != NULL &&
								   fstate.result->type == jbvArray &&
								   fstate.result->val.array.nElems == 1);
							decoded_r[r_idx] = fstate.result->val.array.elems[0];
							has_decoded_r[r_idx] = true;
						}
						else
						{
							avro_skip_binary(wf->type, ptr, end);
						}
					}

					pushJsonbValue(state, WJB_BEGIN_OBJECT, NULL);
					for (int ri = 0; ri < r_schema->nfields; ri++)
					{
						AvroFieldDef *rf = &r_schema->fields[ri];
						JsonbValue	kval;

						CHECK_FOR_INTERRUPTS();
						kval.type = jbvString;
						kval.val.string.val = rf->name;
						kval.val.string.len = (int) strlen(rf->name);
						pushJsonbValue(state, WJB_KEY, &kval);

						if (has_decoded_r && has_decoded_r[ri])
						{
							pushJsonbValue(state, WJB_VALUE, &decoded_r[ri]);
						}
						else if (rf->has_default && rf->default_jsonb != NULL)
						{
							avro_emit_default_jsonb(rf->default_jsonb, state, WJB_VALUE);
						}
						else
						{
							ereport(ERROR,
									(errcode(ERRCODE_DATA_CORRUPTED),
									 errmsg("Avro reader field \"%s\" missing from writer record and has no default",
											rf->name)));
						}
					}
					pushJsonbValue(state, WJB_END_OBJECT, NULL);
					if (decoded_r)
						pfree(decoded_r);
					if (has_decoded_r)
						pfree(has_decoded_r);
				}
				break;
			}

		case AVRO_UNION:
			break;
	}
}

static Jsonb *
avro_decode_root_to_jsonb(AvroSchemaNode *w_schema, AvroSchemaNode *r_schema,
						  const uint8 **ptr, const uint8 *end)
{
	JsonbInState state = {0};
	Jsonb	   *jb;

	pushJsonbValue(&state, WJB_BEGIN_ARRAY, NULL);
	avro_decode_with_token(w_schema, r_schema, ptr, end, &state, WJB_ELEM);
	pushJsonbValue(&state, WJB_END_ARRAY, NULL);
	jb = JsonbValueToJsonb(state.result);

	{
		JsonbIterator *it = JsonbIteratorInit(&jb->root);
		JsonbValue	v;
		JsonbIteratorToken tok;

		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_BEGIN_ARRAY);
		tok = JsonbIteratorNext(&it, &v, true);
		Assert(tok == WJB_ELEM);

		if (v.type == jbvBinary)
		{
			Jsonb	   *unwrapped = avro_jbv_to_jsonb(CurrentMemoryContext, &v);

			pfree(jb);
			return unwrapped;
		}
		else
		{
			jb->root.header |= JB_FSCALAR;
			return jb;
		}
	}
}

static Datum
avro_decode_to_sql_datum(AvroSchemaNode *w_schema, AvroSchemaNode *r_schema,
						 const uint8 **ptr, const uint8 *end,
						 Oid resulttype, bool *isnull)
{
	check_stack_depth();
	CHECK_FOR_INTERRUPTS();

	*isnull = false;

	if (resulttype == JSONBOID)
	{
		Jsonb	   *jb = avro_decode_root_to_jsonb(w_schema, r_schema, ptr, end);

		return JsonbPGetDatum(jb);
	}
	if (resulttype == JSONOID)
	{
		Jsonb	   *jb = avro_decode_root_to_jsonb(w_schema, r_schema, ptr, end);
		char	   *cstr = JsonbToCString(NULL, &jb->root, VARSIZE(jb));
		text	   *txt = cstring_to_text(cstr);

		pfree(cstr);
		pfree(jb);
		return PointerGetDatum(txt);
	}

	if (w_schema->kind == AVRO_UNION)
	{
		int64		bidx = avro_read_varint64(ptr, end, 5);

		if (bidx < 0 || bidx >= w_schema->nbranches)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("corrupted Avro union branch index %lld", (long long) bidx)));
		return avro_decode_to_sql_datum(w_schema->branches[bidx], r_schema,
										ptr, end, resulttype, isnull);
	}

	if (r_schema->kind == AVRO_UNION)
	{
		for (int i = 0; i < r_schema->nbranches; i++)
		{
			if (avro_schemas_compatible(w_schema, r_schema->branches[i]))
				return avro_decode_to_sql_datum(w_schema, r_schema->branches[i],
												ptr, end, resulttype, isnull);
		}
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Avro writer type \"%s\" incompatible with reader union",
						avro_type_kind_name(w_schema->kind))));
	}

	if (r_schema->kind == AVRO_NULL)
	{
		*isnull = true;
		return (Datum) 0;
	}

	switch (r_schema->kind)
	{
		case AVRO_BOOLEAN:
			{
				uint8		b;

				if (*ptr >= end)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated Avro boolean")));
				b = *(*ptr)++;
				if (b > 1)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted Avro boolean byte 0x%02x", b)));
				return BoolGetDatum(b == 1);
			}

		case AVRO_INT:
		case AVRO_LONG:
			{
				int64		lv = avro_read_varint64(ptr, end, (w_schema->kind == AVRO_INT) ? 5 : 10);

				if (resulttype == INT2OID)
					return Int16GetDatum((int16) lv);
				if (resulttype == INT4OID)
					return Int32GetDatum((int32) lv);
				if (resulttype == INT8OID)
					return Int64GetDatum(lv);
				ereport(ERROR,
						(errcode(ERRCODE_DATATYPE_MISMATCH),
						 errmsg("cannot decode Avro integer into SQL type %s",
								format_type_be(resulttype))));
				return (Datum) 0;
			}

		case AVRO_FLOAT:
			{
				float4		fv;

				if (w_schema->kind == AVRO_INT || w_schema->kind == AVRO_LONG)
					fv = (float4) avro_read_varint64(ptr, end, 10);
				else
				{
					if (end - *ptr < 4)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro float")));
					memcpy(&fv, *ptr, 4);
					*ptr += 4;
				}
				if (resulttype == FLOAT8OID)
					return Float8GetDatum((float8) fv);
				return Float4GetDatum(fv);
			}

		case AVRO_DOUBLE:
			{
				float8		dv;

				if (w_schema->kind == AVRO_INT || w_schema->kind == AVRO_LONG)
					dv = (float8) avro_read_varint64(ptr, end, 10);
				else if (w_schema->kind == AVRO_FLOAT)
				{
					float4		fv;

					if (end - *ptr < 4)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro float")));
					memcpy(&fv, *ptr, 4);
					*ptr += 4;
					dv = (float8) fv;
				}
				else
				{
					if (end - *ptr < 8)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("truncated Avro double")));
					memcpy(&dv, *ptr, 8);
					*ptr += 8;
				}
				if (resulttype == FLOAT4OID)
					return Float4GetDatum((float4) dv);
				return Float8GetDatum(dv);
			}

		case AVRO_STRING:
		case AVRO_BYTES:
			{
				int64		slen = avro_read_varint64(ptr, end, 10);
				bytea	   *out;

				if (slen < 0 || end - *ptr < slen)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated Avro string/bytes")));
				out = (bytea *) palloc(VARHDRSZ + slen);
				SET_VARSIZE(out, VARHDRSZ + slen);
				if (slen > 0)
					memcpy(VARDATA(out), *ptr, slen);
				*ptr += slen;
				return PointerGetDatum(out);
			}

		case AVRO_FIXED:
			{
				bytea	   *out;

				if (end - *ptr < r_schema->fixed_size)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("truncated Avro fixed bytes")));
				out = (bytea *) palloc(VARHDRSZ + r_schema->fixed_size);
				SET_VARSIZE(out, VARHDRSZ + r_schema->fixed_size);
				memcpy(VARDATA(out), *ptr, r_schema->fixed_size);
				*ptr += r_schema->fixed_size;
				return PointerGetDatum(out);
			}

		case AVRO_ENUM:
			{
				int64		sidx = avro_read_varint64(ptr, end, 5);

				if (sidx < 0 || sidx >= w_schema->nsymbols)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("corrupted Avro enum index %lld", (long long) sidx)));
				return PointerGetDatum(cstring_to_text(w_schema->symbols[sidx]));
			}

		case AVRO_ARRAY:
			{
				Oid			elem_type = get_element_type(resulttype);
				int16		elmlen;
				bool		elmbyval;
				char		elmalign;
				int			cap = 16;
				int			n = 0;
				Datum	   *elems;
				bool	   *nulls;
				ArrayType  *arr;

				if (!OidIsValid(elem_type))
					ereport(ERROR,
							(errcode(ERRCODE_DATATYPE_MISMATCH),
							 errmsg("cannot decode Avro array into non-array SQL type %s",
									format_type_be(resulttype))));

				get_typlenbyvalalign(elem_type, &elmlen, &elmbyval, &elmalign);
				elems = (Datum *) palloc(sizeof(Datum) * cap);
				nulls = (bool *) palloc(sizeof(bool) * cap);

				for (;;)
				{
					int64		cnt = avro_read_varint64(ptr, end, 10);

					CHECK_FOR_INTERRUPTS();
					if (cnt == 0)
						break;
					if (cnt < 0)
					{
						cnt = -cnt;
						(void) avro_read_varint64(ptr, end, 10);
					}
					for (int64 i = 0; i < cnt; i++)
					{
						CHECK_FOR_INTERRUPTS();
						if (n >= cap)
						{
							cap *= 2;
							elems = (Datum *) repalloc(elems, sizeof(Datum) * cap);
							nulls = (bool *) repalloc(nulls, sizeof(bool) * cap);
						}
						elems[n] = avro_decode_to_sql_datum(w_schema->items, r_schema->items,
															ptr, end, elem_type, &nulls[n]);
						n++;
					}
				}

				if (n == 0)
					arr = construct_empty_array(elem_type);
				else
				{
					int			dims[1];
					int			lbs[1];

					dims[0] = n;
					lbs[0] = 1;
					arr = construct_md_array(elems, nulls, 1, dims, lbs,
											 elem_type, elmlen, elmbyval, elmalign);
				}
				pfree(elems);
				pfree(nulls);
				return PointerGetDatum(arr);
			}

		case AVRO_RECORD:
			{
				TupleDesc	tupdesc = lookup_rowtype_tupdesc(resulttype, -1);
				Datum	   *vals = (Datum *) palloc0(sizeof(Datum) * tupdesc->natts);
				bool	   *nulls = (bool *) palloc(sizeof(bool) * tupdesc->natts);
				bool	   *has_decoded_r = NULL;
				HeapTuple	tuple;
				Datum		res_datum;

				memset(nulls, true, sizeof(bool) * tupdesc->natts);
				if (r_schema->nfields > 0)
					has_decoded_r = (bool *) palloc0(sizeof(bool) * r_schema->nfields);

				for (int wi = 0; wi < w_schema->nfields; wi++)
				{
					AvroFieldDef *wf = &w_schema->fields[wi];
					int			r_idx = -1;
					int			att_idx = -1;

					CHECK_FOR_INTERRUPTS();
					for (int ri = 0; ri < r_schema->nfields; ri++)
					{
						if (strcmp(r_schema->fields[ri].name, wf->name) == 0)
						{
							r_idx = ri;
							break;
						}
					}

					if (r_idx >= 0)
					{
						for (int a = 0; a < tupdesc->natts; a++)
						{
							Form_pg_attribute attr = TupleDescAttr(tupdesc, a);

							if (!attr->attisdropped && strcmp(NameStr(attr->attname), wf->name) == 0)
							{
								att_idx = a;
								break;
							}
						}
					}

					if (r_idx >= 0 && att_idx >= 0)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, att_idx);
						AvroSchemaNode *rf_type = r_schema->fields[r_idx].type;

						vals[att_idx] = avro_decode_to_sql_datum(wf->type, rf_type,
																 ptr, end,
																 attr->atttypid,
																 &nulls[att_idx]);
						has_decoded_r[r_idx] = true;
					}
					else
					{
						if (r_idx >= 0)
							has_decoded_r[r_idx] = true;
						avro_skip_binary(wf->type, ptr, end);
					}
				}

				for (int ri = 0; ri < r_schema->nfields; ri++)
				{
					AvroFieldDef *rf = &r_schema->fields[ri];
					int			att_idx = -1;

					CHECK_FOR_INTERRUPTS();
					if (has_decoded_r && has_decoded_r[ri])
						continue;

					if (!rf->has_default || rf->default_jsonb == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("Avro reader field \"%s\" missing from writer record and has no default",
										rf->name)));

					for (int a = 0; a < tupdesc->natts; a++)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, a);

						if (!attr->attisdropped && strcmp(NameStr(attr->attname), rf->name) == 0)
						{
							att_idx = a;
							break;
						}
					}

					if (att_idx >= 0)
					{
						Form_pg_attribute attr = TupleDescAttr(tupdesc, att_idx);
						StringInfoData def_buf;
						const uint8 *def_ptr;
						const uint8 *def_end;

						initStringInfo(&def_buf);
						avro_encode_jsonb_datum(&def_buf, rf->type, rf->default_jsonb);
						def_ptr = (const uint8 *) def_buf.data;
						def_end = def_ptr + def_buf.len;
						vals[att_idx] = avro_decode_to_sql_datum(rf->type, rf->type,
																 &def_ptr, def_end,
																 attr->atttypid,
																 &nulls[att_idx]);
						pfree(def_buf.data);
					}
				}

				if (has_decoded_r)
					pfree(has_decoded_r);
				tuple = heap_form_tuple(tupdesc, vals, nulls);
				res_datum = heap_copy_tuple_as_datum(tuple, tupdesc);
				heap_freetuple(tuple);
				ReleaseTupleDesc(tupdesc);
				pfree(vals);
				pfree(nulls);
				return res_datum;
			}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("cannot decode Avro type \"%s\" into SQL type %s",
							avro_type_kind_name(r_schema->kind),
							format_type_be(resulttype))));
			return (Datum) 0;
	}
}

/* ========================================================================
 *		3. Single-Stage Fused Avro + Zstd Handlers (avro_zstd)
 * ========================================================================
 */

typedef struct AvroZstdEncState
{
	AvroSchemaNode *writer_schema;
	ZstdEncState zstd_enc;
} AvroZstdEncState;

typedef struct AvroZstdDecState
{
	AvroDecPair avro_pair;
	ZstdDecState zstd_dec;
} AvroZstdDecState;

static bool
avro_zstd_bytes_are_schema(EncodingContext ectx, const char *data, Size len)
{
	const char *s_opt = EncodingContextGetOption(ectx, "schema");
	const char *w_opt = EncodingContextGetOption(ectx, "writer_schema");
	const char *r_opt = EncodingContextGetOption(ectx, "reader_schema");
	bool		is_schema = false;

	if (s_opt == NULL && w_opt == NULL && r_opt == NULL)
		return true;

	if (s_opt != NULL && strlen(s_opt) == len && memcmp(s_opt, data, len) == 0)
		is_schema = true;
	if (w_opt != NULL && strlen(w_opt) == len && memcmp(w_opt, data, len) == 0)
		is_schema = true;
	if (r_opt != NULL && strlen(r_opt) == len && memcmp(r_opt, data, len) == 0)
		is_schema = true;

	if (s_opt)
		pfree((void *) s_opt);
	if (w_opt)
		pfree((void *) w_opt);
	if (r_opt)
		pfree((void *) r_opt);

	return is_schema;
}

static void
avro_zstd_extract_dict(EncodingContext ectx, bool for_decode,
					   const char **dict_ptr, Size *dict_len,
					   bytea **opt_dict_bytea)
{
	const char *dict_opt;

	*dict_ptr = NULL;
	*dict_len = 0;
	*opt_dict_bytea = NULL;

	dict_opt = EncodingContextGetOption(ectx, "dictionary");
	if (dict_opt != NULL)
	{
		Datum		bdatum = DirectFunctionCall1(byteain, CStringGetDatum(dict_opt));

		pfree((void *) dict_opt);
		*opt_dict_bytea = DatumGetByteaP(bdatum);
		if (VARSIZE_ANY_EXHDR(*opt_dict_bytea) > 0)
		{
			*dict_ptr = VARDATA_ANY(*opt_dict_bytea);
			*dict_len = VARSIZE_ANY_EXHDR(*opt_dict_bytea);
			return;
		}
	}

	if (for_decode && ectx->dectab != NULL && ectx->dectab_size > 0)
	{
		const char *dptr = VARDATA_ANY(ectx->dectab);
		Size		dlen = (Size) ectx->dectab_size;

		if (!avro_zstd_bytes_are_schema(ectx, dptr, dlen))
		{
			*dict_ptr = dptr;
			*dict_len = dlen;
			return;
		}
	}

	if (ectx->enctab != NULL && ectx->enctab_size > 0)
	{
		const char *eptr = VARDATA_ANY(ectx->enctab);
		Size		elen = (Size) ectx->enctab_size;

		if (!avro_zstd_bytes_are_schema(ectx, eptr, elen))
		{
			*dict_ptr = eptr;
			*dict_len = elen;
			return;
		}
	}
}

static AvroZstdEncState *
get_avro_zstd_enc_state(EncodingContext ectx)
{
	AvroZstdEncState *state;
	char	   *w_str;
	int			level;
	bool		checksum;
	bool		dict_id_flag;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_zstd_loaded();

	if (ectx->enc_opaque != NULL)
		return (AvroZstdEncState *) ectx->enc_opaque;

	w_str = avro_get_schema_string_from_ectx(ectx, false);
	level = zstd_get_compression_level(ectx);
	checksum = zstd_get_bool_option(ectx, "checksum", false);
	dict_id_flag = zstd_get_bool_option(ectx, "dict_id_flag", true);
	avro_zstd_extract_dict(ectx, false, &dict_ptr, &dict_len, &opt_dict);

	state = (AvroZstdEncState *) MemoryContextAllocZero(ectx->mcxt, sizeof(AvroZstdEncState));
	state->writer_schema = avro_compile_schema_cstr(ectx->mcxt, w_str);
	pfree(w_str);

	state->zstd_enc.level = level;
	state->zstd_enc.checksum = checksum;
	state->zstd_enc.dict_id_flag = dict_id_flag;
	state->zstd_enc.cctx = zstd_api.createCCtx();
	if (state->zstd_enc.cctx == NULL)
	{
		if (opt_dict)
			pfree(opt_dict);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD_CCtx")));
	}

	if (dict_ptr != NULL && dict_len > 0)
	{
		state->zstd_enc.cdict = zstd_api.createCDict(dict_ptr, dict_len, level);
		if (state->zstd_enc.cdict == NULL)
		{
			zstd_api.freeCCtx(state->zstd_enc.cctx);
			state->zstd_enc.cctx = NULL;
			if (opt_dict)
				pfree(opt_dict);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ZSTD_CDict")));
		}
	}

	if (opt_dict)
		pfree(opt_dict);

	state->zstd_enc.cb.func = zstd_enc_state_cleanup;
	state->zstd_enc.cb.arg = &state->zstd_enc;
	MemoryContextRegisterResetCallback(ectx->mcxt, &state->zstd_enc.cb);

	ectx->enc_opaque = state;
	return state;
}

static AvroZstdDecState *
get_avro_zstd_dec_state(EncodingContext ectx)
{
	AvroZstdDecState *state;
	char	   *w_str;
	char	   *r_str;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_zstd_loaded();

	if (ectx->dec_opaque != NULL)
		return (AvroZstdDecState *) ectx->dec_opaque;

	w_str = avro_get_schema_string_from_ectx(ectx, false);
	r_str = avro_get_schema_string_from_ectx(ectx, true);
	avro_zstd_extract_dict(ectx, true, &dict_ptr, &dict_len, &opt_dict);

	state = (AvroZstdDecState *) MemoryContextAllocZero(ectx->mcxt, sizeof(AvroZstdDecState));
	state->avro_pair.writer_schema = avro_compile_schema_cstr(ectx->mcxt, w_str);
	if (strcmp(w_str, r_str) == 0)
		state->avro_pair.reader_schema = state->avro_pair.writer_schema;
	else
		state->avro_pair.reader_schema = avro_compile_schema_cstr(ectx->mcxt, r_str);

	pfree(w_str);
	pfree(r_str);

	state->zstd_dec.dctx = zstd_api.createDCtx();
	if (state->zstd_dec.dctx == NULL)
	{
		if (opt_dict)
			pfree(opt_dict);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD_DCtx")));
	}

	if (dict_ptr != NULL && dict_len > 0)
	{
		state->zstd_dec.ddict = zstd_api.createDDict(dict_ptr, dict_len);
		if (state->zstd_dec.ddict == NULL)
		{
			zstd_api.freeDCtx(state->zstd_dec.dctx);
			state->zstd_dec.dctx = NULL;
			if (opt_dict)
				pfree(opt_dict);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ZSTD_DDict")));
		}
	}

	if (opt_dict)
		pfree(opt_dict);

	state->zstd_dec.cb.func = zstd_dec_state_cleanup;
	state->zstd_dec.cb.arg = &state->zstd_dec;
	MemoryContextRegisterResetCallback(ectx->mcxt, &state->zstd_dec.cb);

	ectx->dec_opaque = state;
	return state;
}

/*
 * pg_avro_zstd_context_encode(internal, anyelement) RETURNS bytea
 */
Datum
pg_avro_zstd_context_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	AvroZstdEncState *state;
	StringInfoData avro_buf;
	bytea	   *result;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_avro_zstd_context_encode called with NULL EncodingContext")));

	if (!OidIsValid(valtype))
		valtype = OidIsValid(ectx->target_typid) ? ectx->target_typid : JSONBOID;

	state = get_avro_zstd_enc_state(ectx);
	initStringInfo(&avro_buf);

	avro_encode_sql_datum(&avro_buf, state->writer_schema, val, false, valtype);
	result = zstd_compress_bytes_with_state(&state->zstd_enc, avro_buf.data, (Size) avro_buf.len);
	pfree(avro_buf.data);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_avro_zstd_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_avro_zstd_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	AvroZstdDecState *state;
	bytea	   *avro_bytes;
	const uint8 *ptr;
	const uint8 *end;
	bool		isnull = false;
	Datum		res;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_avro_zstd_context_decode called with NULL EncodingContext")));

	state = get_avro_zstd_dec_state(ectx);

	if (!OidIsValid(resulttype) ||
		(resulttype == BYTEAOID && !OidIsValid(ectx->target_typid) &&
		 state->avro_pair.reader_schema->kind != AVRO_BYTES &&
		 state->avro_pair.reader_schema->kind != AVRO_FIXED))
	{
		resulttype = OidIsValid(ectx->target_typid) ? ectx->target_typid : JSONBOID;
	}

	avro_bytes = zstd_decompress_bytes_with_state(&state->zstd_dec,
												  VARDATA_ANY(encoded),
												  VARSIZE_ANY_EXHDR(encoded));
	ptr = (const uint8 *) VARDATA_ANY(avro_bytes);
	end = ptr + VARSIZE_ANY_EXHDR(avro_bytes);

	res = avro_decode_to_sql_datum(state->avro_pair.writer_schema,
								   state->avro_pair.reader_schema,
								   &ptr, end, resulttype, &isnull);
	if (ptr != end)
	{
		pfree(avro_bytes);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Avro+Zstd payload has %td trailing unconsumed bytes", end - ptr)));
	}

	pfree(avro_bytes);
	if (isnull)
		PG_RETURN_NULL();
	return res;
}
