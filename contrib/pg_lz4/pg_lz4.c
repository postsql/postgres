/*-------------------------------------------------------------------------
 *
 * pg_lz4.c
 *	  LZ4 context compression, decompression, and sliding-window dictionary
 *	  training for PostgreSQL encoding contexts.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_lz4/pg_lz4.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifndef WIN32
#include <dlfcn.h>
#endif

#ifndef RTLD_LOCAL
#define RTLD_LOCAL 0
#endif

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

PG_FUNCTION_INFO_V1(pg_lz4_context_encode);
PG_FUNCTION_INFO_V1(pg_lz4_context_decode);
PG_FUNCTION_INFO_V1(pg_lz4_train_dictionary);

typedef struct Lz4LibApi
{
	bool		loaded;
	int			(*compressBound) (int inputSize);
	int			(*compress_default) (const char *src, char *dst, int srcSize, int dstCapacity);
	int			(*decompress_safe) (const char *src, char *dst, int compressedSize, int dstCapacity);
	void	   *(*createStream) (void);
	int			(*freeStream) (void *streamPtr);
	void		(*resetStream_fast) (void *streamPtr);
	int			(*loadDict) (void *streamPtr, const char *dictionary, int dictSize);
	void		(*attach_dictionary) (void *workingStream, const void *dictionaryStream);
	int			(*compress_fast_continue) (void *streamPtr, const char *src, char *dst, int srcSize, int dstCapacity, int acceleration);
	int			(*decompress_safe_usingDict) (const char *src, char *dst, int compressedSize, int maxOutputSize, const char *dictStart, int dictSize);
} Lz4LibApi;

static Lz4LibApi lz4_api = {0};

/*
 * Self-contained standard LZ4 block + dictionary software fallback used when
 * the host environment does not provide an external liblz4 shared library
 * (e.g., Windows CI builds with -Dauto_features=disabled).
 */
typedef struct FbLz4Stream
{
	const char *dict;
	int			dict_size;
} FbLz4Stream;

static uint32
fb_lz4_read_u32(const uint8 *p)
{
	uint32		v;

	memcpy(&v, p, sizeof(uint32));
	return v;
}

static int
fb_lz4_compressBound(int inputSize)
{
	if (inputSize < 0)
		return 0;
	return inputSize + (inputSize / 255) + 16;
}

static int
fb_lz4_compress_ext(const char *src, char *dst, int srcSize, int dstCapacity,
					const char *dict, int dictSize)
{
	const uint8 *ip = (const uint8 *) src;
	const uint8 *anchor = ip;
	const uint8 *const iend = ip + srcSize;
	const uint8 *const mflimit = iend - 12;
	const uint8 *const matchlimit = iend - 5;
	uint8	   *op = (uint8 *) dst;
	uint8	   *const oend = op + dstCapacity;
	int32		htab[4096];
	const uint8 *dict_base = NULL;
	int			use_dict_size = 0;

	if (srcSize < 0 || dstCapacity <= 0)
		return 0;

	if (srcSize == 0)
	{
		if (dstCapacity < 1)
			return 0;
		*op++ = 0;
		return 1;
	}

	for (int i = 0; i < 4096; i++)
		htab[i] = PG_INT32_MIN;

	if (dict != NULL && dictSize >= 4)
	{
		use_dict_size = (dictSize > 65535) ? 65535 : dictSize;
		dict_base = (const uint8 *) dict + (dictSize - use_dict_size);
		for (int p = -use_dict_size; p <= -4; p++)
		{
			uint32		v = fb_lz4_read_u32(dict_base + use_dict_size + p);
			uint32		h = (v * 2654435761U) >> 20;

			htab[h] = p;
		}
	}

	if (srcSize >= 13)
	{
		while (ip <= mflimit)
		{
			int			cur = (int) (ip - (const uint8 *) src);
			uint32		v = fb_lz4_read_u32(ip);
			uint32		h = (v * 2654435761U) >> 20;
			int32		ref = htab[h];
			int			offset = cur - ref;
			int			mlen = 0;

			htab[h] = cur;

			if (ref > PG_INT32_MIN && offset > 0 && offset <= 65535)
			{
				if (ref >= 0)
				{
					const uint8 *refp = (const uint8 *) src + ref;

					if (fb_lz4_read_u32(refp) == v)
					{
						int			max_m = (int) (matchlimit - ip);

						mlen = 4;
						while (mlen < max_m && refp[mlen] == ip[mlen])
							mlen++;
					}
				}
				else if (dict_base != NULL && ref + 4 <= 0)
				{
					const uint8 *refp = dict_base + use_dict_size + ref;

					if (fb_lz4_read_u32(refp) == v)
					{
						int			max_dict_m = -ref;
						int			max_src_m = (int) (matchlimit - ip);
						int			max_m = (max_dict_m < max_src_m) ? max_dict_m : max_src_m;

						mlen = 4;
						while (mlen < max_m && refp[mlen] == ip[mlen])
							mlen++;
					}
				}
			}

			if (mlen >= 4)
			{
				int			lit_len = (int) (ip - anchor);
				int			m_code = mlen - 4;
				uint8	   *token_ptr;

				if (op + 1 + (lit_len / 255) + 1 + lit_len + 2 + (m_code / 255) + 1 > oend)
					return 0;

				token_ptr = op++;
				if (lit_len >= 15)
				{
					int			rem = lit_len - 15;

					*token_ptr = (uint8) (15 << 4);
					while (rem >= 255)
					{
						*op++ = 255;
						rem -= 255;
					}
					*op++ = (uint8) rem;
				}
				else
					*token_ptr = (uint8) (lit_len << 4);

				if (lit_len > 0)
				{
					memcpy(op, anchor, lit_len);
					op += lit_len;
				}

				*op++ = (uint8) (offset & 0xFF);
				*op++ = (uint8) ((offset >> 8) & 0xFF);

				if (m_code >= 15)
				{
					int			rem = m_code - 15;

					*token_ptr |= 15;
					while (rem >= 255)
					{
						*op++ = 255;
						rem -= 255;
					}
					*op++ = (uint8) rem;
				}
				else
					*token_ptr |= (uint8) m_code;

				ip += mlen;
				anchor = ip;
			}
			else
			{
				ip++;
			}
		}
	}

	{
		int			last_lit = (int) (iend - anchor);
		uint8	   *token_ptr;

		if (op + 1 + (last_lit / 255) + 1 + last_lit > oend)
			return 0;

		token_ptr = op++;
		if (last_lit >= 15)
		{
			int			rem = last_lit - 15;

			*token_ptr = (uint8) (15 << 4);
			while (rem >= 255)
			{
				*op++ = 255;
				rem -= 255;
			}
			*op++ = (uint8) rem;
		}
		else
			*token_ptr = (uint8) (last_lit << 4);

		if (last_lit > 0)
		{
			memcpy(op, anchor, last_lit);
			op += last_lit;
		}
	}

	return (int) (op - (uint8 *) dst);
}

static int
fb_lz4_decompress_safe_usingDict(const char *src, char *dst, int compressedSize,
								 int maxOutputSize, const char *dictStart, int dictSize)
{
	const uint8 *ip = (const uint8 *) src;
	const uint8 *const iend = ip + compressedSize;
	uint8	   *op = (uint8 *) dst;
	uint8	   *const oend = op + maxOutputSize;
	const uint8 *dict_base = NULL;
	int			use_dict_size = 0;

	if (compressedSize < 0 || maxOutputSize < 0)
		return -1;
	if (compressedSize == 0)
		return (maxOutputSize == 0) ? 0 : -1;

	if (dictStart != NULL && dictSize > 0)
	{
		use_dict_size = dictSize;
		dict_base = (const uint8 *) dictStart;
	}

	while (ip < iend)
	{
		uint8		token = *ip++;
		int			lit_len = token >> 4;
		int			match_len;
		int			offset;
		int			cur_out;

		if (lit_len == 15)
		{
			uint8		s;

			do
			{
				if (ip >= iend)
					return -(int) (ip - (const uint8 *) src) - 1;
				s = *ip++;
				lit_len += s;
				if (lit_len > maxOutputSize)
					return -(int) (ip - (const uint8 *) src) - 1;
			} while (s == 255);
		}

		if (ip + lit_len > iend || op + lit_len > oend)
			return -(int) (ip - (const uint8 *) src) - 1;

		if (lit_len > 0)
		{
			memcpy(op, ip, lit_len);
			ip += lit_len;
			op += lit_len;
		}

		if (ip == iend)
		{
			if ((token & 0x0F) != 0)
				return -(int) (ip - (const uint8 *) src) - 1;
			break;
		}

		if (ip + 2 > iend)
			return -(int) (ip - (const uint8 *) src) - 1;

		offset = (int) ip[0] | ((int) ip[1] << 8);
		ip += 2;
		if (offset <= 0)
			return -(int) (ip - (const uint8 *) src) - 1;

		match_len = (token & 0x0F) + 4;
		if ((token & 0x0F) == 15)
		{
			uint8		s;

			do
			{
				if (ip >= iend)
					return -(int) (ip - (const uint8 *) src) - 1;
				s = *ip++;
				match_len += s;
				if (match_len > maxOutputSize)
					return -(int) (ip - (const uint8 *) src) - 1;
			} while (s == 255);
		}

		if (op + match_len > oend)
			return -(int) (ip - (const uint8 *) src) - 1;

		cur_out = (int) (op - (uint8 *) dst);
		if (offset <= cur_out)
		{
			const uint8 *match = op - offset;

			for (int k = 0; k < match_len; k++)
				*op++ = *match++;
		}
		else
		{
			int			dict_back = offset - cur_out;
			int			dict_off;
			int			copy_from_dict;

			if (dict_base == NULL || dict_back > use_dict_size)
				return -(int) (ip - (const uint8 *) src) - 1;

			dict_off = use_dict_size - dict_back;
			copy_from_dict = use_dict_size - dict_off;
			if (copy_from_dict > match_len)
				copy_from_dict = match_len;

			memcpy(op, dict_base + dict_off, copy_from_dict);
			op += copy_from_dict;
			match_len -= copy_from_dict;

			if (match_len > 0)
			{
				const uint8 *match = (const uint8 *) dst;

				for (int k = 0; k < match_len; k++)
					*op++ = *match++;
			}
		}
	}

	return (int) (op - (uint8 *) dst);
}

static int
fb_lz4_compress_default(const char *src, char *dst, int srcSize, int dstCapacity)
{
	return fb_lz4_compress_ext(src, dst, srcSize, dstCapacity, NULL, 0);
}

static int
fb_lz4_decompress_safe(const char *src, char *dst, int compressedSize, int dstCapacity)
{
	return fb_lz4_decompress_safe_usingDict(src, dst, compressedSize, dstCapacity, NULL, 0);
}

static void *
fb_lz4_createStream(void)
{
	return calloc(1, sizeof(FbLz4Stream));
}

static int
fb_lz4_freeStream(void *streamPtr)
{
	if (streamPtr != NULL)
		free(streamPtr);
	return 0;
}

static void
fb_lz4_resetStream_fast(void *streamPtr)
{
	if (streamPtr != NULL)
		memset(streamPtr, 0, sizeof(FbLz4Stream));
}

static int
fb_lz4_loadDict(void *streamPtr, const char *dictionary, int dictSize)
{
	FbLz4Stream *s = (FbLz4Stream *) streamPtr;

	if (s == NULL)
		return 0;
	s->dict = dictionary;
	s->dict_size = dictSize;
	return dictSize;
}

static void
fb_lz4_attach_dictionary(void *workingStream, const void *dictionaryStream)
{
	FbLz4Stream *ws = (FbLz4Stream *) workingStream;
	const FbLz4Stream *ds = (const FbLz4Stream *) dictionaryStream;

	if (ws != NULL && ds != NULL)
	{
		ws->dict = ds->dict;
		ws->dict_size = ds->dict_size;
	}
}

static int
fb_lz4_compress_fast_continue(void *streamPtr, const char *src, char *dst,
							  int srcSize, int dstCapacity, int acceleration)
{
	FbLz4Stream *ws = (FbLz4Stream *) streamPtr;

	(void) acceleration;
	return fb_lz4_compress_ext(src, dst, srcSize, dstCapacity,
							   ws ? ws->dict : NULL,
							   ws ? ws->dict_size : 0);
}

static void
ensure_lz4_loaded(void)
{
	static const char *const lz4_lib_candidates[] = {
		"liblz4.so.1",
		"liblz4.so",
		"liblz4.1.dylib",
		"liblz4.dylib",
		"/opt/local/lib/liblz4.1.dylib",
		"/opt/local/lib/liblz4.dylib",
		"/opt/homebrew/lib/liblz4.1.dylib",
		"/opt/homebrew/lib/liblz4.dylib",
		"/usr/local/lib/liblz4.1.dylib",
		"/usr/local/lib/liblz4.dylib",
		"liblz4.dll",
		"lz4.dll",
		"C:\\msys64\\ucrt64\\bin\\liblz4.dll",
		"C:\\mingw64\\bin\\liblz4.dll",
		NULL
	};
	void	   *handle = NULL;

	if (lz4_api.loaded)
		return;

	if (getenv("PG_COMPRESSION_FORCE_FALLBACK") == NULL)
	{
#ifndef WIN32
		void	   *self = dlopen(NULL, RTLD_NOW | RTLD_LOCAL);

		if (self != NULL && dlsym(self, "LZ4_compressBound") != NULL)
			handle = self;
#endif

		for (int i = 0; handle == NULL && lz4_lib_candidates[i] != NULL; i++)
			handle = dlopen(lz4_lib_candidates[i], RTLD_NOW | RTLD_LOCAL);
	}

	if (handle != NULL)
	{
		lz4_api.compressBound = dlsym(handle, "LZ4_compressBound");
		lz4_api.compress_default = dlsym(handle, "LZ4_compress_default");
		lz4_api.decompress_safe = dlsym(handle, "LZ4_decompress_safe");
		lz4_api.createStream = dlsym(handle, "LZ4_createStream");
		lz4_api.freeStream = dlsym(handle, "LZ4_freeStream");
		lz4_api.resetStream_fast = dlsym(handle, "LZ4_resetStream_fast");
		lz4_api.loadDict = dlsym(handle, "LZ4_loadDict");
		lz4_api.attach_dictionary = dlsym(handle, "LZ4_attach_dictionary");
		lz4_api.compress_fast_continue = dlsym(handle, "LZ4_compress_fast_continue");
		lz4_api.decompress_safe_usingDict = dlsym(handle, "LZ4_decompress_safe_usingDict");

		if (lz4_api.compressBound && lz4_api.compress_default &&
			lz4_api.decompress_safe && lz4_api.createStream &&
			lz4_api.freeStream && lz4_api.resetStream_fast &&
			lz4_api.loadDict && lz4_api.attach_dictionary &&
			lz4_api.compress_fast_continue && lz4_api.decompress_safe_usingDict)
		{
			lz4_api.loaded = true;
			return;
		}
	}

	/* Fall back to built-in standard LZ4 implementation */
	lz4_api.compressBound = fb_lz4_compressBound;
	lz4_api.compress_default = fb_lz4_compress_default;
	lz4_api.decompress_safe = fb_lz4_decompress_safe;
	lz4_api.createStream = fb_lz4_createStream;
	lz4_api.freeStream = fb_lz4_freeStream;
	lz4_api.resetStream_fast = fb_lz4_resetStream_fast;
	lz4_api.loadDict = fb_lz4_loadDict;
	lz4_api.attach_dictionary = fb_lz4_attach_dictionary;
	lz4_api.compress_fast_continue = fb_lz4_compress_fast_continue;
	lz4_api.decompress_safe_usingDict = fb_lz4_decompress_safe_usingDict;
	lz4_api.loaded = true;
}

typedef struct Lz4EncState
{
	void	   *dict_stream;
	void	   *work_stream;
	char	   *dict_buf;
	int			dict_len;
	int			acceleration;
	MemoryContextCallback cb;
} Lz4EncState;

typedef struct Lz4DecState
{
	char	   *dict_buf;
	int			dict_len;
} Lz4DecState;

static void
lz4_enc_state_cleanup(void *arg)
{
	Lz4EncState *state = (Lz4EncState *) arg;

	if (state->work_stream != NULL)
	{
		lz4_api.freeStream(state->work_stream);
		state->work_stream = NULL;
	}
	if (state->dict_stream != NULL)
	{
		lz4_api.freeStream(state->dict_stream);
		state->dict_stream = NULL;
	}
}

static void
lz4_extract_dictionary_bytes(EncodingContext ectx, bool for_decode,
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

static int
lz4_get_acceleration(EncodingContext ectx)
{
	const char *acc_str;
	char	   *endptr = NULL;
	long		val;

	acc_str = EncodingContextGetOption(ectx, "acceleration");
	if (acc_str == NULL)
		acc_str = EncodingContextGetOption(ectx, "level");
	if (acc_str == NULL)
		acc_str = EncodingContextGetOption(ectx, "compression_level");
	if (acc_str == NULL)
		return 1;

	if (pg_strcasecmp(acc_str, "default") == 0)
	{
		pfree((void *) acc_str);
		return 1;
	}

	errno = 0;
	val = strtol(acc_str, &endptr, 10);
	if (errno != 0 || endptr == acc_str || *endptr != '\0' ||
		val < 1 || val > 65537)
	{
		char	   *copy = pstrdup(acc_str);

		pfree((void *) acc_str);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid lz4 acceleration \"%s\" (expected integer >= 1)",
						copy)));
	}

	pfree((void *) acc_str);
	return (int) val;
}

static Lz4EncState *
get_lz4_enc_state(EncodingContext ectx)
{
	Lz4EncState *state;
	int			acceleration;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_lz4_loaded();

	if (ectx->enc_opaque != NULL)
		return (Lz4EncState *) ectx->enc_opaque;

	acceleration = lz4_get_acceleration(ectx);
	lz4_extract_dictionary_bytes(ectx, false, &dict_ptr, &dict_len, &opt_dict);

	state = (Lz4EncState *) MemoryContextAllocZero(ectx->mcxt, sizeof(Lz4EncState));
	state->acceleration = acceleration;

	if (dict_ptr != NULL && dict_len > 0)
	{
		/* LZ4 uses at most the last 64 KB of a dictionary */
		Size		use_len = Min(dict_len, (Size) 65536);
		const char *use_ptr = dict_ptr + (dict_len - use_len);

		state->dict_buf = (char *) MemoryContextAlloc(ectx->mcxt, use_len);
		memcpy(state->dict_buf, use_ptr, use_len);
		state->dict_len = (int) use_len;

		state->dict_stream = lz4_api.createStream();
		state->work_stream = lz4_api.createStream();
		if (state->dict_stream == NULL || state->work_stream == NULL)
		{
			if (state->dict_stream)
				lz4_api.freeStream(state->dict_stream);
			if (state->work_stream)
				lz4_api.freeStream(state->work_stream);
			if (opt_dict)
				pfree(opt_dict);
			ereport(ERROR,
					(errcode(ERRCODE_OUT_OF_MEMORY),
					 errmsg("could not create LZ4_stream_t")));
		}

		lz4_api.loadDict(state->dict_stream, state->dict_buf, state->dict_len);

		state->cb.func = lz4_enc_state_cleanup;
		state->cb.arg = state;
		MemoryContextRegisterResetCallback(ectx->mcxt, &state->cb);
	}

	if (opt_dict)
		pfree(opt_dict);

	ectx->enc_opaque = state;
	return state;
}

static Lz4DecState *
get_lz4_dec_state(EncodingContext ectx)
{
	Lz4DecState *state;
	const char *dict_ptr = NULL;
	Size		dict_len = 0;
	bytea	   *opt_dict = NULL;

	ensure_lz4_loaded();

	if (ectx->dec_opaque != NULL)
		return (Lz4DecState *) ectx->dec_opaque;

	lz4_extract_dictionary_bytes(ectx, true, &dict_ptr, &dict_len, &opt_dict);

	state = (Lz4DecState *) MemoryContextAllocZero(ectx->mcxt, sizeof(Lz4DecState));
	if (dict_ptr != NULL && dict_len > 0)
	{
		Size		use_len = Min(dict_len, (Size) 65536);
		const char *use_ptr = dict_ptr + (dict_len - use_len);

		state->dict_buf = (char *) MemoryContextAlloc(ectx->mcxt, use_len);
		memcpy(state->dict_buf, use_ptr, use_len);
		state->dict_len = (int) use_len;
	}

	if (opt_dict)
		pfree(opt_dict);

	ectx->dec_opaque = state;
	return state;
}

/*
 * pg_lz4_context_encode(internal, anyelement) RETURNS bytea
 *
 * Wire format: 4-byte int32 raw_len + LZ4 compressed block.
 */
Datum
pg_lz4_context_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const char *src_bytes;
	int32		src_len;
	struct varlena *detoasted = NULL;
	Lz4EncState *state;
	int			bound;
	bytea	   *result;
	char	   *dst_ptr;
	int			comp_len;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_lz4_context_encode called with NULL EncodingContext")));

	if (!OidIsValid(valtype))
		valtype = BYTEAOID;

	get_typlenbyvalalign(valtype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		detoasted = pg_detoast_datum_packed(DatumGetPointer(val));
		src_bytes = VARDATA_ANY(detoasted);
		src_len = (int32) VARSIZE_ANY_EXHDR(detoasted);
	}
	else if (typbyval)
	{
		src_bytes = (const char *) &val;
		src_len = typlen;
	}
	else
	{
		src_bytes = (const char *) DatumGetPointer(val);
		src_len = typlen;
	}

	state = get_lz4_enc_state(ectx);
	bound = lz4_api.compressBound(src_len);
	result = (bytea *) palloc(VARHDRSZ + sizeof(int32) + bound);
	dst_ptr = VARDATA(result);
	memcpy(dst_ptr, &src_len, sizeof(int32));

	if (state->dict_stream != NULL && state->work_stream != NULL)
	{
		lz4_api.resetStream_fast(state->work_stream);
		lz4_api.attach_dictionary(state->work_stream, state->dict_stream);
		comp_len = lz4_api.compress_fast_continue(state->work_stream,
												  src_bytes,
												  dst_ptr + sizeof(int32),
												  src_len,
												  bound,
												  state->acceleration);
	}
	else
	{
		comp_len = lz4_api.compress_default(src_bytes,
											dst_ptr + sizeof(int32),
											src_len,
											bound);
	}

	if (comp_len <= 0 && src_len > 0)
	{
		pfree(result);
		if (detoasted != NULL && (Pointer) detoasted != DatumGetPointer(val))
			pfree(detoasted);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("lz4 context compression failed")));
	}

	SET_VARSIZE(result, VARHDRSZ + sizeof(int32) + comp_len);

	if (detoasted != NULL && (Pointer) detoasted != DatumGetPointer(val))
		pfree(detoasted);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_lz4_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_lz4_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const char *src_ptr;
	int32		encoded_len;
	int32		raw_len;
	Lz4DecState *state;
	bytea	   *out_varlena;
	char	   *dst_bytes;
	int			dec_len;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_lz4_context_decode called with NULL EncodingContext")));

	if (!OidIsValid(resulttype))
		resulttype = BYTEAOID;

	src_ptr = VARDATA_ANY(encoded);
	encoded_len = (int32) VARSIZE_ANY_EXHDR(encoded);

	if (encoded_len < (int32) sizeof(int32))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted lz4 context payload (length %d < 4)", encoded_len)));

	memcpy(&raw_len, src_ptr, sizeof(int32));
	if (raw_len < 0 || (Size) raw_len > MaxAllocSize - VARHDRSZ)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted lz4 context raw length %d", raw_len)));

	state = get_lz4_dec_state(ectx);
	out_varlena = (bytea *) palloc(VARHDRSZ + raw_len);
	SET_VARSIZE(out_varlena, VARHDRSZ + raw_len);
	dst_bytes = VARDATA(out_varlena);

	if (state->dict_buf != NULL && state->dict_len > 0)
	{
		dec_len = lz4_api.decompress_safe_usingDict(src_ptr + sizeof(int32),
													dst_bytes,
													encoded_len - (int32) sizeof(int32),
													raw_len,
													state->dict_buf,
													state->dict_len);
	}
	else
	{
		dec_len = lz4_api.decompress_safe(src_ptr + sizeof(int32),
										  dst_bytes,
										  encoded_len - (int32) sizeof(int32),
										  raw_len);
	}

	if (dec_len != raw_len)
	{
		pfree(out_varlena);
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("lz4 context decompression failed (expected %d bytes, got %d)",
						raw_len, dec_len)));
	}

	get_typlenbyvalalign(resulttype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		PG_RETURN_POINTER(out_varlena);
	}
	else if (typbyval)
	{
		Datum		res = 0;

		if (raw_len != typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded lz4 payload length %d does not match type length %d",
							raw_len, typlen)));
		memcpy(&res, dst_bytes, typlen);
		pfree(out_varlena);
		return res;
	}
	else
	{
		void	   *ptr = palloc(typlen);

		if (raw_len != typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded lz4 payload length %d does not match type length %d",
							raw_len, typlen)));
		memcpy(ptr, dst_bytes, typlen);
		pfree(out_varlena);
		PG_RETURN_POINTER(ptr);
	}
}

/*
 * pg_lz4_train_dictionary(samples bytea[], max_dict_size int4 DEFAULT 65536) RETURNS bytea
 *
 * Builds a raw LZ4 dictionary (up to 64 KB) from an array of sample byteas.
 * LZ4 uses a raw sliding-window dictionary of up to 64 KB where matches closer
 * to the end of the dictionary have smaller offsets. We extract deduplicated
 * representative substrings and sample templates, placing the highest-frequency
 * common segments at the end of the dictionary buffer.
 */
Datum
pg_lz4_train_dictionary(PG_FUNCTION_ARGS)
{
	ArrayType  *samples_arr;
	int32		max_dict_size = 65536;
	int32		target_size;
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	StringInfoData dict_buf;
	bytea	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	samples_arr = PG_GETARG_ARRAYTYPE_P(0);
	if (!PG_ARGISNULL(1))
		max_dict_size = PG_GETARG_INT32(1);

	if (max_dict_size <= 0 || max_dict_size > 104857600)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid lz4 max_dict_size %d (must be between 1 and 104857600)",
						max_dict_size)));

	target_size = Min(max_dict_size, 65536);

	deconstruct_array(samples_arr, BYTEAOID, -1, false, TYPALIGN_INT, &elems, &nulls, &nelems);
	if (nelems <= 0)
	{
		pfree(elems);
		pfree(nulls);
		result = (bytea *) palloc(VARHDRSZ);
		SET_VARSIZE(result, VARHDRSZ);
		PG_RETURN_BYTEA_P(result);
	}

	initStringInfo(&dict_buf);

	/*
	 * Pass 1: Collect unique 32-byte chunks across samples to capture common
	 * structural headers, field names, and recurring values without bloating
	 * the dictionary with identical repetitions.
	 */
	for (int i = 0; i < nelems && dict_buf.len < target_size; i++)
	{
		bytea	   *sample;
		const char *sdata;
		int			slen;

		CHECK_FOR_INTERRUPTS();
		if (nulls[i])
			continue;

		sample = DatumGetByteaPP(elems[i]);
		sdata = VARDATA_ANY(sample);
		slen = (int) VARSIZE_ANY_EXHDR(sample);
		if (slen <= 0)
			continue;

		if (slen <= 64)
		{
			bool		found = false;

			for (int pos = 0; pos + slen <= dict_buf.len; pos++)
			{
				if (memcmp(dict_buf.data + pos, sdata, slen) == 0)
				{
					found = true;
					break;
				}
			}
			if (!found && dict_buf.len + slen <= target_size)
				appendBinaryStringInfo(&dict_buf, sdata, slen);
		}
		else
		{
			for (int off = 0; off < slen && dict_buf.len < target_size; off += 32)
			{
				int			clen = Min(64, slen - off);
				bool		found = false;

				CHECK_FOR_INTERRUPTS();
				if (clen < 8)
					break;

				for (int pos = 0; pos + clen <= dict_buf.len; pos++)
				{
					if (memcmp(dict_buf.data + pos, sdata + off, clen) == 0)
					{
						found = true;
						break;
					}
				}
				if (!found)
				{
					int			add_len = Min(clen, target_size - dict_buf.len);

					appendBinaryStringInfo(&dict_buf, sdata + off, add_len);
				}
			}
		}
	}

	/*
	 * Pass 2: Append a full representative sample at the very end of the
	 * dictionary buffer (if space permits) so LZ4 can match whole contiguous
	 * record templates at the smallest offset from the end of the window.
	 */
	for (int i = 0; i < nelems && dict_buf.len < target_size; i++)
	{
		bytea	   *sample;
		const char *sdata;
		int			slen;
		int			rem;

		CHECK_FOR_INTERRUPTS();
		if (nulls[i])
			continue;

		sample = DatumGetByteaPP(elems[i]);
		sdata = VARDATA_ANY(sample);
		slen = (int) VARSIZE_ANY_EXHDR(sample);
		if (slen <= 0)
			continue;

		rem = target_size - dict_buf.len;
		if (rem > 0)
		{
			int			copy_len = Min(slen, rem);

			appendBinaryStringInfo(&dict_buf, sdata, copy_len);
			break;
		}
	}

	result = (bytea *) palloc(VARHDRSZ + dict_buf.len);
	SET_VARSIZE(result, VARHDRSZ + dict_buf.len);
	if (dict_buf.len > 0)
		memcpy(VARDATA(result), dict_buf.data, dict_buf.len);

	pfree(dict_buf.data);
	pfree(elems);
	pfree(nulls);

	PG_RETURN_BYTEA_P(result);
}
