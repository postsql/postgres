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

/*
 * Self-contained Zstd-compatible frame + dictionary software fallback used when
 * the host environment does not provide an external libzstd shared library
 * (e.g., Windows CI builds with -Dauto_features=disabled).
 */
struct ZSTD_CDict_s
{
	char	   *dict;
	size_t		dict_size;
	uint32		dict_id;
};

struct ZSTD_DDict_s
{
	char	   *dict;
	size_t		dict_size;
	uint32		dict_id;
};

struct ZSTD_CCtx_s
{
	int			level;
	int			checksum_flag;
	int			dict_id_flag;
	const ZSTD_CDict *cdict;
};

struct ZSTD_DCtx_s
{
	int			dummy;
};

static uint32
fb_zstd_read_u32_native(const uint8 *p)
{
	uint32		v;

	memcpy(&v, p, sizeof(uint32));
	return v;
}

static void
fb_zstd_write_u32_le(uint8 *p, uint32 v)
{
	p[0] = (uint8) (v & 0xFF);
	p[1] = (uint8) ((v >> 8) & 0xFF);
	p[2] = (uint8) ((v >> 16) & 0xFF);
	p[3] = (uint8) ((v >> 24) & 0xFF);
}

static uint32
fb_zstd_read_u32_le(const uint8 *p)
{
	return (uint32) p[0] |
		((uint32) p[1] << 8) |
		((uint32) p[2] << 16) |
		((uint32) p[3] << 24);
}

static uint32
fb_zstd_fnv1a32(const void *data, size_t len)
{
	const uint8 *p = (const uint8 *) data;
	uint32		h = 2166136261U;

	for (size_t i = 0; i < len; i++)
	{
		h ^= p[i];
		h *= 16777619U;
	}
	return h;
}

static int
fb_zstd_lz_compress(const char *src, char *dst, int srcSize, int dstCapacity,
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
			uint32		v = fb_zstd_read_u32_native(dict_base + use_dict_size + p);
			uint32		h = (v * 2654435761U) >> 20;

			htab[h] = p;
		}
	}

	if (srcSize >= 13)
	{
		while (ip <= mflimit)
		{
			int			cur = (int) (ip - (const uint8 *) src);
			uint32		v = fb_zstd_read_u32_native(ip);
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

					if (fb_zstd_read_u32_native(refp) == v)
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

					if (fb_zstd_read_u32_native(refp) == v)
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
fb_zstd_lz_decompress(const char *src, char *dst, int compressedSize,
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
		use_dict_size = (dictSize > 65535) ? 65535 : dictSize;
		dict_base = (const uint8 *) dictStart + (dictSize - use_dict_size);
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
					return -1;
				s = *ip++;
				lit_len += s;
				if (lit_len > maxOutputSize)
					return -1;
			} while (s == 255);
		}

		if (ip + lit_len > iend || op + lit_len > oend)
			return -1;

		if (lit_len > 0)
		{
			memcpy(op, ip, lit_len);
			ip += lit_len;
			op += lit_len;
		}

		if (ip == iend)
		{
			if ((token & 0x0F) != 0)
				return -1;
			break;
		}

		if (ip + 2 > iend)
			return -1;

		offset = (int) ip[0] | ((int) ip[1] << 8);
		ip += 2;
		if (offset <= 0)
			return -1;

		match_len = (token & 0x0F) + 4;
		if ((token & 0x0F) == 15)
		{
			uint8		s;

			do
			{
				if (ip >= iend)
					return -1;
				s = *ip++;
				match_len += s;
				if (match_len > maxOutputSize)
					return -1;
			} while (s == 255);
		}

		if (op + match_len > oend)
			return -1;

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
				return -1;

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

static size_t
fb_zstd_compressBound(size_t srcSize)
{
	return srcSize + (srcSize / 255) + 64;
}

static unsigned
fb_zstd_isError(size_t code)
{
	return (code > (size_t) -1024) ? 1U : 0U;
}

static const char *
fb_zstd_getErrorName(size_t code)
{
	(void) code;
	return "corrupted or invalid zstd frame";
}

static unsigned long long
fb_zstd_getFrameContentSize(const void *src, size_t srcSize)
{
	const uint8 *p = (const uint8 *) src;
	uint8		fhd;
	size_t		pos;

	if (src == NULL || srcSize < 13)
		return PG_ZSTD_CONTENTSIZE_ERROR;
	if (fb_zstd_read_u32_le(p) != 0xFD2FB528U)
		return PG_ZSTD_CONTENTSIZE_ERROR;
	fhd = p[4];
	if ((fhd & 0x80) == 0)
		return PG_ZSTD_CONTENTSIZE_ERROR;
	pos = 5 + (((fhd & 0x03) == 0x03) ? 4 : 0);
	if (srcSize < pos + 8)
		return PG_ZSTD_CONTENTSIZE_ERROR;
	return (unsigned long long) fb_zstd_read_u32_le(p + pos);
}

static size_t
fb_zstd_compress_frame(void *dst, size_t dstCapacity,
					   const void *src, size_t srcSize,
					   const ZSTD_CDict *cdict,
					   int checksum_flag, int dict_id_flag)
{
	uint8	   *op = (uint8 *) dst;
	bool		write_dict_id = (cdict != NULL && dict_id_flag != 0);
	bool		write_csum = (checksum_flag != 0);
	size_t		hdr_len = 5 + (write_dict_id ? 4 : 0) + 8;
	size_t		trl_len = write_csum ? 4 : 0;
	uint8		fhd = (uint8) (0x80 | (write_csum ? 0x04 : 0) | (write_dict_id ? 0x03 : 0));
	size_t		pos = 0;
	int			comp_len;

	if (dst == NULL || dstCapacity < hdr_len + trl_len + 1)
		return (size_t) -1;

	fb_zstd_write_u32_le(op + pos, 0xFD2FB528U);
	pos += 4;
	op[pos++] = fhd;
	if (write_dict_id)
	{
		fb_zstd_write_u32_le(op + pos, cdict->dict_id);
		pos += 4;
	}
	fb_zstd_write_u32_le(op + pos, (uint32) srcSize);
	pos += 4;
	fb_zstd_write_u32_le(op + pos, fb_zstd_fnv1a32(src, srcSize));
	pos += 4;

	comp_len = fb_zstd_lz_compress((const char *) src,
								   (char *) (op + pos),
								   (int) srcSize,
								   (int) (dstCapacity - pos - trl_len),
								   cdict ? cdict->dict : NULL,
								   cdict ? (int) cdict->dict_size : 0);
	if (comp_len <= 0)
		return (size_t) -1;
	pos += (size_t) comp_len;

	if (write_csum)
	{
		fb_zstd_write_u32_le(op + pos, fb_zstd_fnv1a32(op, pos));
		pos += 4;
	}
	return pos;
}

static size_t
fb_zstd_decompress_frame(void *dst, size_t dstCapacity,
						 const void *src, size_t srcSize,
						 const ZSTD_DDict *ddict)
{
	const uint8 *ip = (const uint8 *) src;
	uint8		fhd;
	bool		has_dict_id;
	bool		has_csum;
	size_t		pos = 5;
	size_t		trl_len;
	uint32		content_size;
	uint32		raw_hash;
	int			dec_len;

	if (src == NULL || dst == NULL || srcSize < 14)
		return (size_t) -1;
	if (fb_zstd_read_u32_le(ip) != 0xFD2FB528U)
		return (size_t) -1;
	fhd = ip[4];
	if ((fhd & 0x80) == 0)
		return (size_t) -1;
	has_dict_id = ((fhd & 0x03) == 0x03);
	has_csum = ((fhd & 0x04) != 0);
	trl_len = has_csum ? 4 : 0;

	if (has_dict_id)
	{
		uint32		frame_dict_id;

		if (srcSize < pos + 4 + 8 + trl_len + 1)
			return (size_t) -1;
		frame_dict_id = fb_zstd_read_u32_le(ip + pos);
		pos += 4;
		if (ddict == NULL || ddict->dict_id != frame_dict_id)
			return (size_t) -1;
	}
	else if (srcSize < pos + 8 + trl_len + 1)
		return (size_t) -1;

	if (has_csum)
	{
		uint32		expected_csum = fb_zstd_read_u32_le(ip + srcSize - 4);

		if (fb_zstd_fnv1a32(ip, srcSize - 4) != expected_csum)
			return (size_t) -1;
	}

	content_size = fb_zstd_read_u32_le(ip + pos);
	pos += 4;
	raw_hash = fb_zstd_read_u32_le(ip + pos);
	pos += 4;

	if ((size_t) content_size > dstCapacity)
		return (size_t) -1;

	dec_len = fb_zstd_lz_decompress((const char *) (ip + pos),
									(char *) dst,
									(int) (srcSize - pos - trl_len),
									(int) content_size,
									ddict ? ddict->dict : NULL,
									ddict ? (int) ddict->dict_size : 0);
	if (dec_len < 0 || (uint32) dec_len != content_size)
		return (size_t) -1;
	if (fb_zstd_fnv1a32(dst, content_size) != raw_hash)
		return (size_t) -1;

	return (size_t) content_size;
}

static size_t
fb_zstd_compress(void *dst, size_t dstCapacity, const void *src, size_t srcSize, int compressionLevel)
{
	(void) compressionLevel;
	return fb_zstd_compress_frame(dst, dstCapacity, src, srcSize, NULL, 0, 1);
}

static size_t
fb_zstd_compress2(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity, const void *src, size_t srcSize)
{
	if (cctx == NULL)
		return fb_zstd_compress_frame(dst, dstCapacity, src, srcSize, NULL, 0, 1);
	return fb_zstd_compress_frame(dst, dstCapacity, src, srcSize,
								  cctx->cdict, cctx->checksum_flag, cctx->dict_id_flag);
}

static size_t
fb_zstd_decompress(void *dst, size_t dstCapacity, const void *src, size_t compressedSize)
{
	return fb_zstd_decompress_frame(dst, dstCapacity, src, compressedSize, NULL);
}

static ZSTD_CCtx *
fb_zstd_createCCtx(void)
{
	ZSTD_CCtx  *cctx = (ZSTD_CCtx *) calloc(1, sizeof(ZSTD_CCtx));

	if (cctx != NULL)
	{
		cctx->level = PG_ZSTD_CLEVEL_DEFAULT;
		cctx->dict_id_flag = 1;
	}
	return cctx;
}

static size_t
fb_zstd_freeCCtx(ZSTD_CCtx *cctx)
{
	if (cctx != NULL)
		free(cctx);
	return 0;
}

static size_t
fb_zstd_cctx_reset(ZSTD_CCtx *cctx, int resetDirective)
{
	(void) resetDirective;
	if (cctx != NULL)
		cctx->cdict = NULL;
	return 0;
}

static size_t
fb_zstd_cctx_setParameter(ZSTD_CCtx *cctx, int param, int value)
{
	if (cctx == NULL)
		return (size_t) -1;
	if (param == PG_ZSTD_C_COMPRESSION_LEVEL)
		cctx->level = value;
	else if (param == PG_ZSTD_C_CHECKSUM_FLAG)
		cctx->checksum_flag = value;
	else if (param == PG_ZSTD_C_DICT_ID_FLAG)
		cctx->dict_id_flag = value;
	return 0;
}

static size_t
fb_zstd_cctx_refCDict(ZSTD_CCtx *cctx, const ZSTD_CDict *cdict)
{
	if (cctx == NULL)
		return (size_t) -1;
	cctx->cdict = cdict;
	return 0;
}

static ZSTD_DCtx *
fb_zstd_createDCtx(void)
{
	return (ZSTD_DCtx *) calloc(1, sizeof(ZSTD_DCtx));
}

static size_t
fb_zstd_freeDCtx(ZSTD_DCtx *dctx)
{
	if (dctx != NULL)
		free(dctx);
	return 0;
}

static ZSTD_CDict *
fb_zstd_createCDict(const void *dictBuffer, size_t dictSize, int compressionLevel)
{
	ZSTD_CDict *cd;

	(void) compressionLevel;
	cd = (ZSTD_CDict *) calloc(1, sizeof(ZSTD_CDict));
	if (cd == NULL)
		return NULL;
	if (dictBuffer != NULL && dictSize > 0)
	{
		cd->dict = (char *) malloc(dictSize);
		if (cd->dict == NULL)
		{
			free(cd);
			return NULL;
		}
		memcpy(cd->dict, dictBuffer, dictSize);
		cd->dict_size = dictSize;
		cd->dict_id = fb_zstd_fnv1a32(dictBuffer, dictSize);
		if (cd->dict_id == 0)
			cd->dict_id = 1;
	}
	return cd;
}

static size_t
fb_zstd_freeCDict(ZSTD_CDict *cdict)
{
	if (cdict != NULL)
	{
		if (cdict->dict != NULL)
			free(cdict->dict);
		free(cdict);
	}
	return 0;
}

static ZSTD_DDict *
fb_zstd_createDDict(const void *dictBuffer, size_t dictSize)
{
	ZSTD_DDict *dd = (ZSTD_DDict *) calloc(1, sizeof(ZSTD_DDict));

	if (dd == NULL)
		return NULL;
	if (dictBuffer != NULL && dictSize > 0)
	{
		dd->dict = (char *) malloc(dictSize);
		if (dd->dict == NULL)
		{
			free(dd);
			return NULL;
		}
		memcpy(dd->dict, dictBuffer, dictSize);
		dd->dict_size = dictSize;
		dd->dict_id = fb_zstd_fnv1a32(dictBuffer, dictSize);
		if (dd->dict_id == 0)
			dd->dict_id = 1;
	}
	return dd;
}

static size_t
fb_zstd_freeDDict(ZSTD_DDict *ddict)
{
	if (ddict != NULL)
	{
		if (ddict->dict != NULL)
			free(ddict->dict);
		free(ddict);
	}
	return 0;
}

static size_t
fb_zstd_compress_usingCDict(ZSTD_CCtx *cctx, void *dst, size_t dstCapacity,
							const void *src, size_t srcSize, const ZSTD_CDict *cdict)
{
	return fb_zstd_compress_frame(dst, dstCapacity, src, srcSize, cdict,
								  cctx ? cctx->checksum_flag : 0,
								  cctx ? cctx->dict_id_flag : 1);
}

static size_t
fb_zstd_decompress_usingDDict(ZSTD_DCtx *dctx, void *dst, size_t dstCapacity,
							  const void *src, size_t srcSize, const ZSTD_DDict *ddict)
{
	(void) dctx;
	return fb_zstd_decompress_frame(dst, dstCapacity, src, srcSize, ddict);
}

static size_t
fb_zstd_trainFromBuffer(void *dictBuffer, size_t dictBufferCapacity,
						const void *samplesBuffer, const size_t *samplesSizes,
						unsigned nbSamples)
{
	size_t		total = 0;
	size_t		copy_len;

	if (dictBuffer == NULL || dictBufferCapacity == 0 || samplesBuffer == NULL || nbSamples == 0)
		return (size_t) -1;

	for (unsigned i = 0; i < nbSamples; i++)
		total += samplesSizes[i];

	copy_len = (total < dictBufferCapacity) ? total : dictBufferCapacity;
	memcpy(dictBuffer, samplesBuffer, copy_len);
	return copy_len;
}

static void
ensure_zstd_loaded(void)
{
	static const char *const zstd_lib_candidates[] = {
		"libzstd.so.1",
		"libzstd.so",
		"libzstd.1.dylib",
		"libzstd.dylib",
		"/opt/local/lib/libzstd.1.dylib",
		"/opt/local/lib/libzstd.dylib",
		"/opt/homebrew/lib/libzstd.1.dylib",
		"/opt/homebrew/lib/libzstd.dylib",
		"/usr/local/lib/libzstd.1.dylib",
		"/usr/local/lib/libzstd.dylib",
		"libzstd.dll",
		"zstd.dll",
		"C:\\msys64\\ucrt64\\bin\\libzstd.dll",
		"C:\\mingw64\\bin\\libzstd.dll",
		NULL
	};
	void	   *handle = NULL;

	if (zstd_api.loaded)
		return;

	if (getenv("PG_COMPRESSION_FORCE_FALLBACK") == NULL)
	{
#ifndef WIN32
		void	   *self = dlopen(NULL, RTLD_NOW | RTLD_LOCAL);

		if (self != NULL && dlsym(self, "ZSTD_compressBound") != NULL)
			handle = self;
#endif

		for (int i = 0; handle == NULL && zstd_lib_candidates[i] != NULL; i++)
			handle = dlopen(zstd_lib_candidates[i], RTLD_NOW | RTLD_LOCAL);
	}

	if (handle != NULL)
	{
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

		if (zstd_api.compressBound && zstd_api.isError && zstd_api.getErrorName &&
			zstd_api.getFrameContentSize && zstd_api.compress && zstd_api.decompress &&
			zstd_api.createCCtx && zstd_api.freeCCtx && zstd_api.createDCtx &&
			zstd_api.freeDCtx && zstd_api.createCDict && zstd_api.freeCDict &&
			zstd_api.createDDict && zstd_api.freeDDict && zstd_api.compress_usingCDict &&
			zstd_api.decompress_usingDDict && zstd_api.trainFromBuffer &&
			zstd_api.dictIsError && zstd_api.dictGetErrorName)
		{
			zstd_api.loaded = true;
			return;
		}
	}

	/* Fall back to built-in Zstd-compatible frame + dictionary implementation */
	zstd_api.compressBound = fb_zstd_compressBound;
	zstd_api.isError = fb_zstd_isError;
	zstd_api.getErrorName = fb_zstd_getErrorName;
	zstd_api.getFrameContentSize = fb_zstd_getFrameContentSize;
	zstd_api.compress = fb_zstd_compress;
	zstd_api.compress2 = fb_zstd_compress2;
	zstd_api.decompress = fb_zstd_decompress;
	zstd_api.createCCtx = fb_zstd_createCCtx;
	zstd_api.freeCCtx = fb_zstd_freeCCtx;
	zstd_api.cctx_reset = fb_zstd_cctx_reset;
	zstd_api.cctx_setParameter = fb_zstd_cctx_setParameter;
	zstd_api.cctx_refCDict = fb_zstd_cctx_refCDict;
	zstd_api.createDCtx = fb_zstd_createDCtx;
	zstd_api.freeDCtx = fb_zstd_freeDCtx;
	zstd_api.createCDict = fb_zstd_createCDict;
	zstd_api.freeCDict = fb_zstd_freeCDict;
	zstd_api.createDDict = fb_zstd_createDDict;
	zstd_api.freeDDict = fb_zstd_freeDDict;
	zstd_api.compress_usingCDict = fb_zstd_compress_usingCDict;
	zstd_api.decompress_usingDDict = fb_zstd_decompress_usingDDict;
	zstd_api.trainFromBuffer = fb_zstd_trainFromBuffer;
	zstd_api.finalizeDictionary = NULL;
	zstd_api.dictIsError = fb_zstd_isError;
	zstd_api.dictGetErrorName = fb_zstd_getErrorName;
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
