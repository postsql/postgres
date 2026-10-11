/*-------------------------------------------------------------------------
 *
 * pg_zstd.c
 *	  Zstandard (zstd) context compression, decompression, and FastCover
 *	  dictionary training for PostgreSQL encoding contexts.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_zstd/pg_zstd.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <dlfcn.h>

#include "access/detoast.h"
#include "access/encoding_context.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_zstd_context_encode);
PG_FUNCTION_INFO_V1(pg_zstd_context_decode);
PG_FUNCTION_INFO_V1(pg_zstd_train_dictionary);

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
	size_t		(*compressBound) (size_t srcSize);
	unsigned	(*isError) (size_t code);
	const char *(*getErrorName) (size_t code);
	unsigned long long (*getFrameContentSize) (const void *src, size_t srcSize);
	size_t		(*compress) (void *dst, size_t dstCapacity, const void *src, size_t srcSize, int compressionLevel);
	size_t		(*compress2) (ZSTD_CCtx *cctx, void *dst, size_t dstCapacity, const void *src, size_t srcSize);
	size_t		(*decompress) (void *dst, size_t dstCapacity, const void *src, size_t compressedSize);
	ZSTD_CCtx  *(*createCCtx) (void);
	size_t		(*freeCCtx) (ZSTD_CCtx *cctx);
	size_t		(*cctx_reset) (ZSTD_CCtx *cctx, int resetDirective);
	size_t		(*cctx_setParameter) (ZSTD_CCtx *cctx, int param, int value);
	size_t		(*cctx_refCDict) (ZSTD_CCtx *cctx, const ZSTD_CDict *cdict);
	ZSTD_DCtx  *(*createDCtx) (void);
	size_t		(*freeDCtx) (ZSTD_DCtx *dctx);
	ZSTD_CDict *(*createCDict) (const void *dictBuffer, size_t dictSize, int compressionLevel);
	size_t		(*freeCDict) (ZSTD_CDict *cdict);
	ZSTD_DDict *(*createDDict) (const void *dictBuffer, size_t dictSize);
	size_t		(*freeDDict) (ZSTD_DDict *ddict);
	size_t		(*compress_usingCDict) (ZSTD_CCtx *cctx, void *dst, size_t dstCapacity, const void *src, size_t srcSize, const ZSTD_CDict *cdict);
	size_t		(*decompress_usingDDict) (ZSTD_DCtx *dctx, void *dst, size_t dstCapacity, const void *src, size_t srcSize, const ZSTD_DDict *ddict);
	size_t		(*trainFromBuffer) (void *dictBuffer, size_t dictBufferCapacity, const void *samplesBuffer, const size_t *samplesSizes, unsigned nbSamples);
	size_t		(*finalizeDictionary) (void *dstDictBuffer, size_t maxDictSize, const void *dictContent, size_t dictContentSize, const void *samplesBuffer, const size_t *samplesSizes, unsigned nbSamples, ZDICT_params_t parameters);
	unsigned	(*dictIsError) (size_t errorCode);
	const char *(*dictGetErrorName) (size_t errorCode);
} ZstdLibApi;

static ZstdLibApi zstd_api = {0};

static void
ensure_zstd_loaded(void)
{
	void	   *handle;

	if (zstd_api.loaded)
		return;

	handle = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		handle = dlopen("libzstd.so", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not load libzstd shared library: %s", dlerror())));

	zstd_api.compressBound = dlsym(handle, "ZSTD_compressBound");
	zstd_api.isError = dlsym(handle, "ZSTD_isError");
	zstd_api.getErrorName = dlsym(handle, "ZSTD_getErrorName");
	zstd_api.getFrameContentSize = dlsym(handle, "ZSTD_getFrameContentSize");
	zstd_api.compress = dlsym(handle, "ZSTD_compress");
	zstd_api.compress2 = dlsym(handle, "ZSTD_compress2");
	zstd_api.decompress = dlsym(handle, "ZSTD_decompress");
	zstd_api.createCCtx = dlsym(handle, "ZSTD_createCCtx");
	zstd_api.freeCCtx = dlsym(handle, "ZSTD_freeCCtx");
	zstd_api.cctx_reset = dlsym(handle, "ZSTD_CCtx_reset");
	zstd_api.cctx_setParameter = dlsym(handle, "ZSTD_CCtx_setParameter");
	zstd_api.cctx_refCDict = dlsym(handle, "ZSTD_CCtx_refCDict");
	zstd_api.createDCtx = dlsym(handle, "ZSTD_createDCtx");
	zstd_api.freeDCtx = dlsym(handle, "ZSTD_freeDCtx");
	zstd_api.createCDict = dlsym(handle, "ZSTD_createCDict");
	zstd_api.freeCDict = dlsym(handle, "ZSTD_freeCDict");
	zstd_api.createDDict = dlsym(handle, "ZSTD_createDDict");
	zstd_api.freeDDict = dlsym(handle, "ZSTD_freeDDict");
	zstd_api.compress_usingCDict = dlsym(handle, "ZSTD_compress_usingCDict");
	zstd_api.decompress_usingDDict = dlsym(handle, "ZSTD_decompress_usingDDict");
	zstd_api.trainFromBuffer = dlsym(handle, "ZDICT_trainFromBuffer");
	zstd_api.finalizeDictionary = dlsym(handle, "ZDICT_finalizeDictionary");
	zstd_api.dictIsError = dlsym(handle, "ZDICT_isError");
	zstd_api.dictGetErrorName = dlsym(handle, "ZDICT_getErrorName");

	if (!zstd_api.compressBound || !zstd_api.isError || !zstd_api.getErrorName ||
		!zstd_api.getFrameContentSize || !zstd_api.compress || !zstd_api.decompress ||
		!zstd_api.createCCtx || !zstd_api.freeCCtx || !zstd_api.createDCtx ||
		!zstd_api.freeDCtx || !zstd_api.createCDict || !zstd_api.freeCDict ||
		!zstd_api.createDDict || !zstd_api.freeDDict || !zstd_api.compress_usingCDict ||
		!zstd_api.decompress_usingDDict || !zstd_api.trainFromBuffer ||
		!zstd_api.dictIsError || !zstd_api.dictGetErrorName)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("libzstd is missing required dictionary compression symbols")));
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

	if (state->cdict != NULL)
	{
		zstd_api.freeCDict(state->cdict);
		state->cdict = NULL;
	}
	if (state->cctx != NULL)
	{
		zstd_api.freeCCtx(state->cctx);
		state->cctx = NULL;
	}
}

static void
zstd_dec_state_cleanup(void *arg)
{
	ZstdDecState *state = (ZstdDecState *) arg;

	if (state->ddict != NULL)
	{
		zstd_api.freeDDict(state->ddict);
		state->ddict = NULL;
	}
	if (state->dctx != NULL)
	{
		zstd_api.freeDCtx(state->dctx);
		state->dctx = NULL;
	}
}

static int
zstd_get_compression_level(EncodingContext ectx)
{
	const char *lvl_str;
	char	   *endptr = NULL;
	long		val;

	lvl_str = EncodingContextGetOption(ectx, "level");
	if (lvl_str == NULL)
		lvl_str = EncodingContextGetOption(ectx, "compression_level");
	if (lvl_str == NULL)
		return PG_ZSTD_CLEVEL_DEFAULT;

	if (pg_strcasecmp(lvl_str, "default") == 0)
	{
		pfree((void *) lvl_str);
		return PG_ZSTD_CLEVEL_DEFAULT;
	}

	errno = 0;
	val = strtol(lvl_str, &endptr, 10);
	if (errno != 0 || endptr == lvl_str || *endptr != '\0' ||
		val < -131072 || val > 22)
	{
		char	   *copy = pstrdup(lvl_str);

		pfree((void *) lvl_str);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid zstd compression level \"%s\" (expected integer between -131072 and 22)",
						copy)));
	}

	pfree((void *) lvl_str);
	return (int) val;
}

static bool
zstd_get_bool_option(EncodingContext ectx, const char *optname, bool default_val)
{
	const char *val_str = EncodingContextGetOption(ectx, optname);
	bool		res = default_val;

	if (val_str == NULL)
		return default_val;

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
				 errmsg("invalid boolean value \"%s\" for option \"%s\"",
						copy, optname)));
	}

	pfree((void *) val_str);
	return res;
}

static void
zstd_extract_dictionary_bytes(EncodingContext ectx, bool for_decode,
							  const char **dict_ptr, Size *dict_len,
							  bytea **opt_dict_bytea)
{
	*dict_ptr = NULL;
	*dict_len = 0;
	*opt_dict_bytea = NULL;

	if (!for_decode)
	{
		if (ectx->enctab != NULL && ectx->enctab_size > 0)
		{
			*dict_ptr = VARDATA_ANY(ectx->enctab);
			*dict_len = (Size) ectx->enctab_size;
			return;
		}
	}
	else
	{
		bytea	   *dectab = EncodingContextGetDecTab(ectx);

		if (dectab != NULL && VARSIZE_ANY_EXHDR(dectab) > 0)
		{
			*dict_ptr = VARDATA_ANY(dectab);
			*dict_len = VARSIZE_ANY_EXHDR(dectab);
			return;
		}
	}

	/* Also check OPTIONS (dictionary = '...') */
	{
		const char *dict_opt = EncodingContextGetOption(ectx, "dictionary");

		if (dict_opt != NULL)
		{
			Datum		bdatum;

			bdatum = DirectFunctionCall1(byteain, CStringGetDatum(dict_opt));
			pfree((void *) dict_opt);
			*opt_dict_bytea = DatumGetByteaP(bdatum);
			if (VARSIZE_ANY_EXHDR(*opt_dict_bytea) > 0)
			{
				*dict_ptr = VARDATA_ANY(*opt_dict_bytea);
				*dict_len = VARSIZE_ANY_EXHDR(*opt_dict_bytea);
			}
		}
	}
}

static ZstdEncState *
get_zstd_enc_state(EncodingContext ectx)
{
	ZstdEncState *state;
	int			level;
	bool		checksum;
	bool		dict_id_flag;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_zstd_loaded();

	if (ectx->enc_opaque != NULL)
		return (ZstdEncState *) ectx->enc_opaque;

	level = zstd_get_compression_level(ectx);
	checksum = zstd_get_bool_option(ectx, "checksum", false);
	dict_id_flag = zstd_get_bool_option(ectx, "dict_id_flag", true);
	zstd_extract_dictionary_bytes(ectx, false, &dict_ptr, &dict_len, &opt_dict);

	state = (ZstdEncState *) MemoryContextAllocZero(ectx->mcxt, sizeof(ZstdEncState));
	state->level = level;
	state->checksum = checksum;
	state->dict_id_flag = dict_id_flag;
	state->cctx = zstd_api.createCCtx();
	if (state->cctx == NULL)
	{
		if (opt_dict)
			pfree(opt_dict);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD_CCtx")));
	}

	if (dict_ptr != NULL && dict_len > 0)
	{
		state->cdict = zstd_api.createCDict(dict_ptr, dict_len, level);
		if (state->cdict == NULL)
		{
			zstd_api.freeCCtx(state->cctx);
			state->cctx = NULL;
			if (opt_dict)
				pfree(opt_dict);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ZSTD_CDict from dictionary of %zu bytes", dict_len)));
		}
	}

	if (opt_dict)
		pfree(opt_dict);

	state->cb.func = zstd_enc_state_cleanup;
	state->cb.arg = state;
	MemoryContextRegisterResetCallback(ectx->mcxt, &state->cb);

	ectx->enc_opaque = state;
	return state;
}

static ZstdDecState *
get_zstd_dec_state(EncodingContext ectx)
{
	ZstdDecState *state;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_zstd_loaded();

	if (ectx->dec_opaque != NULL)
		return (ZstdDecState *) ectx->dec_opaque;

	zstd_extract_dictionary_bytes(ectx, true, &dict_ptr, &dict_len, &opt_dict);

	state = (ZstdDecState *) MemoryContextAllocZero(ectx->mcxt, sizeof(ZstdDecState));
	state->dctx = zstd_api.createDCtx();
	if (state->dctx == NULL)
	{
		if (opt_dict)
			pfree(opt_dict);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not create ZSTD_DCtx")));
	}

	if (dict_ptr != NULL && dict_len > 0)
	{
		state->ddict = zstd_api.createDDict(dict_ptr, dict_len);
		if (state->ddict == NULL)
		{
			zstd_api.freeDCtx(state->dctx);
			state->dctx = NULL;
			if (opt_dict)
				pfree(opt_dict);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create ZSTD_DDict from dictionary of %zu bytes", dict_len)));
		}
	}

	if (opt_dict)
		pfree(opt_dict);

	state->cb.func = zstd_dec_state_cleanup;
	state->cb.arg = state;
	MemoryContextRegisterResetCallback(ectx->mcxt, &state->cb);

	ectx->dec_opaque = state;
	return state;
}

static bytea *
zstd_compress_bytes_with_state(ZstdEncState *state, const char *src_bytes, Size src_len)
{
	size_t		bound = zstd_api.compressBound(src_len);
	bytea	   *result = (bytea *) palloc(VARHDRSZ + bound);
	size_t		comp_len;

	if (zstd_api.compress2 != NULL && zstd_api.cctx_setParameter != NULL)
	{
		if (zstd_api.cctx_reset != NULL)
			zstd_api.cctx_reset(state->cctx, 1 /* ZSTD_reset_session_only */);
		zstd_api.cctx_setParameter(state->cctx, PG_ZSTD_C_COMPRESSION_LEVEL, state->level);
		zstd_api.cctx_setParameter(state->cctx, PG_ZSTD_C_CHECKSUM_FLAG, state->checksum ? 1 : 0);
		zstd_api.cctx_setParameter(state->cctx, PG_ZSTD_C_DICT_ID_FLAG, state->dict_id_flag ? 1 : 0);
		if (zstd_api.cctx_refCDict != NULL)
			zstd_api.cctx_refCDict(state->cctx, state->cdict);
		if (state->cdict != NULL && zstd_api.cctx_refCDict == NULL)
			comp_len = zstd_api.compress_usingCDict(state->cctx,
													VARDATA(result), bound,
													src_bytes, src_len,
													state->cdict);
		else
			comp_len = zstd_api.compress2(state->cctx,
										  VARDATA(result), bound,
										  src_bytes, src_len);
	}
	else if (state->cdict != NULL)
		comp_len = zstd_api.compress_usingCDict(state->cctx,
												VARDATA(result), bound,
												src_bytes, src_len,
												state->cdict);
	else
		comp_len = zstd_api.compress(VARDATA(result), bound,
									 src_bytes, src_len,
									 state->level);

	if (zstd_api.isError(comp_len))
	{
		pfree(result);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("zstd compression failed: %s",
						zstd_api.getErrorName(comp_len))));
	}

	SET_VARSIZE(result, VARHDRSZ + comp_len);
	return result;
}

static bytea *
zstd_decompress_bytes_with_state(ZstdDecState *state, const char *src_ptr, Size encoded_len)
{
	unsigned long long frame_size;
	Size		out_cap;
	bytea	   *out_varlena;
	size_t		dec_len;

	if (encoded_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted zstd payload: empty input")));

	frame_size = zstd_api.getFrameContentSize(src_ptr, encoded_len);
	if (frame_size == PG_ZSTD_CONTENTSIZE_ERROR)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted zstd frame header")));

	if (frame_size != PG_ZSTD_CONTENTSIZE_UNKNOWN)
	{
		if (frame_size > (unsigned long long) MaxAllocSize - VARHDRSZ)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("zstd frame uncompressed size %llu exceeds maximum allocation size",
							frame_size)));
		out_cap = (Size) frame_size;
		out_varlena = (bytea *) palloc(VARHDRSZ + out_cap);
		if (state->ddict != NULL)
			dec_len = zstd_api.decompress_usingDDict(state->dctx,
													 VARDATA(out_varlena), out_cap,
													 src_ptr, encoded_len,
													 state->ddict);
		else
			dec_len = zstd_api.decompress(VARDATA(out_varlena), out_cap,
										  src_ptr, encoded_len);

		if (zstd_api.isError(dec_len))
		{
			pfree(out_varlena);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("zstd decompression failed: %s",
							zstd_api.getErrorName(dec_len))));
		}
	}
	else
	{
		out_cap = Max(encoded_len * 4, (Size) 65536);
		if (out_cap > MaxAllocSize - VARHDRSZ)
			out_cap = MaxAllocSize - VARHDRSZ;

		for (;;)
		{
			CHECK_FOR_INTERRUPTS();
			out_varlena = (bytea *) palloc(VARHDRSZ + out_cap);
			if (state->ddict != NULL)
				dec_len = zstd_api.decompress_usingDDict(state->dctx,
														 VARDATA(out_varlena), out_cap,
														 src_ptr, encoded_len,
														 state->ddict);
			else
				dec_len = zstd_api.decompress(VARDATA(out_varlena), out_cap,
											  src_ptr, encoded_len);

			if (!zstd_api.isError(dec_len))
				break;

			pfree(out_varlena);
			if (out_cap >= MaxAllocSize - VARHDRSZ)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("zstd decompression failed: %s",
								zstd_api.getErrorName(dec_len))));

			if (out_cap > (MaxAllocSize - VARHDRSZ) / 2)
				out_cap = MaxAllocSize - VARHDRSZ;
			else
				out_cap *= 2;
		}
	}

	SET_VARSIZE(out_varlena, VARHDRSZ + dec_len);
	return out_varlena;
}

/*
 * pg_zstd_context_encode(internal, anyelement) RETURNS bytea
 */
Datum
pg_zstd_context_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const char *src_bytes;
	Size		src_len;
	struct varlena *detoasted = NULL;
	ZstdEncState *state;
	bytea	   *result;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_zstd_context_encode called with NULL EncodingContext")));

	if (!OidIsValid(valtype))
		valtype = BYTEAOID;

	get_typlenbyvalalign(valtype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		detoasted = pg_detoast_datum_packed(DatumGetPointer(val));
		src_bytes = VARDATA_ANY(detoasted);
		src_len = VARSIZE_ANY_EXHDR(detoasted);
	}
	else if (typbyval)
	{
		src_bytes = (const char *) &val;
		src_len = (Size) typlen;
	}
	else
	{
		src_bytes = (const char *) DatumGetPointer(val);
		src_len = (Size) typlen;
	}

	state = get_zstd_enc_state(ectx);
	result = zstd_compress_bytes_with_state(state, src_bytes, src_len);

	if (detoasted != NULL && (Pointer) detoasted != DatumGetPointer(val))
		pfree(detoasted);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_zstd_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_zstd_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	ZstdDecState *state;
	bytea	   *out_varlena;
	Size		raw_len;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_zstd_context_decode called with NULL EncodingContext")));

	if (!OidIsValid(resulttype))
		resulttype = BYTEAOID;

	state = get_zstd_dec_state(ectx);
	out_varlena = zstd_decompress_bytes_with_state(state,
												   VARDATA_ANY(encoded),
												   VARSIZE_ANY_EXHDR(encoded));
	raw_len = VARSIZE_ANY_EXHDR(out_varlena);

	get_typlenbyvalalign(resulttype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		PG_RETURN_POINTER(out_varlena);
	}
	else if (typbyval)
	{
		Datum		res = 0;

		if (raw_len != (Size) typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded zstd payload length %zu does not match type length %d",
							raw_len, typlen)));
		memcpy(&res, VARDATA(out_varlena), typlen);
		pfree(out_varlena);
		return res;
	}
	else
	{
		void	   *ptr = palloc(typlen);

		if (raw_len != (Size) typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded zstd payload length %zu does not match type length %d",
							raw_len, typlen)));
		memcpy(ptr, VARDATA(out_varlena), typlen);
		pfree(out_varlena);
		PG_RETURN_POINTER(ptr);
	}
}

/*
 * pg_zstd_train_dictionary(samples bytea[], max_dict_size int4 DEFAULT 65536) RETURNS bytea
 */
Datum
pg_zstd_train_dictionary(PG_FUNCTION_ARGS)
{
	ArrayType  *samples_arr;
	int32		max_dict_size = 65536;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	StringInfoData samples_buf;
	size_t	   *sample_sizes;
	unsigned	cap_samples;
	unsigned	nb_samples = 0;
	bytea	   *dict_result;
	size_t		trained_size;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	samples_arr = PG_GETARG_ARRAYTYPE_P(0);
	if (!PG_ARGISNULL(1))
		max_dict_size = PG_GETARG_INT32(1);

	if (max_dict_size < 256 || max_dict_size > 104857600)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid zstd max_dict_size %d (must be between 256 and 104857600)",
						max_dict_size)));

	ensure_zstd_loaded();

	deconstruct_array(samples_arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, &nelems);
	if (nelems <= 0)
	{
		pfree(elems);
		pfree(nulls);
		dict_result = (bytea *) palloc(VARHDRSZ);
		SET_VARSIZE(dict_result, VARHDRSZ);
		PG_RETURN_BYTEA_P(dict_result);
	}

	initStringInfo(&samples_buf);
	cap_samples = Max(nelems * 16, 64);
	sample_sizes = (size_t *) palloc(sizeof(size_t) * cap_samples);

	for (int i = 0; i < nelems; i++)
	{
		bytea	   *sample;
		Size		slen;

		CHECK_FOR_INTERRUPTS();
		if (nulls[i])
			continue;

		sample = DatumGetByteaPP(elems[i]);
		slen = VARSIZE_ANY_EXHDR(sample);
		if (slen == 0)
			continue;

		appendBinaryStringInfo(&samples_buf, VARDATA_ANY(sample), (int) slen);
		sample_sizes[nb_samples++] = slen;
	}

	if (nb_samples == 0 || samples_buf.len == 0)
	{
		pfree(samples_buf.data);
		pfree(sample_sizes);
		pfree(elems);
		pfree(nulls);
		dict_result = (bytea *) palloc(VARHDRSZ);
		SET_VARSIZE(dict_result, VARHDRSZ);
		PG_RETURN_BYTEA_P(dict_result);
	}

	/*
	 * ZDICT_trainFromBuffer (FastCover) requires at least ~8 samples and a
	 * minimum corpus size. If the caller supplied only a few small samples,
	 * replicate the sample list up to at least 16 samples and 4 KB so
	 * ZDICT_trainFromBuffer can build a formatted Zstd dictionary.
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
	trained_size = zstd_api.trainFromBuffer(VARDATA(dict_result),
											(size_t) max_dict_size,
											samples_buf.data,
											sample_sizes,
											nb_samples);

	if ((zstd_api.dictIsError(trained_size) || trained_size == 0) &&
		zstd_api.finalizeDictionary != NULL && samples_buf.len >= 256)
	{
		ZDICT_params_t zparams;
		Size		content_len = Min((Size) samples_buf.len, (Size) max_dict_size / 2);

		memset(&zparams, 0, sizeof(zparams));
		zparams.compressionLevel = PG_ZSTD_CLEVEL_DEFAULT;
		if (content_len < 256)
			content_len = Min((Size) samples_buf.len, (Size) max_dict_size);
		trained_size = zstd_api.finalizeDictionary(VARDATA(dict_result),
												   (size_t) max_dict_size,
												   samples_buf.data,
												   content_len,
												   samples_buf.data,
												   sample_sizes,
												   nb_samples,
												   zparams);
	}

	if (zstd_api.dictIsError(trained_size) || trained_size == 0)
	{
		Size		raw_copy_len = Min((Size) samples_buf.len, (Size) max_dict_size);

		memcpy(VARDATA(dict_result), samples_buf.data, raw_copy_len);
		SET_VARSIZE(dict_result, VARHDRSZ + raw_copy_len);
	}
	else
	{
		SET_VARSIZE(dict_result, VARHDRSZ + trained_size);
	}

	pfree(samples_buf.data);
	pfree(sample_sizes);
	pfree(elems);
	pfree(nulls);

	PG_RETURN_BYTEA_P(dict_result);
}
