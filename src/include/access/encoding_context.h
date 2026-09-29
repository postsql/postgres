/*-------------------------------------------------------------------------
 *
 * encoding_context.h
 *	  Definitions and data structures for PostgreSQL encoding contexts.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * src/include/access/encoding_context.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENCODING_CONTEXT_H
#define ENCODING_CONTEXT_H

#include "postgres.h"
#include "fmgr.h"

/* Maximum chain depth for composed encoding contexts to prevent cycles */
#define MAX_ENCODING_CONTEXT_DEPTH	8

/*
 * EncodingContextData
 *
 * Represents an in-memory encoding context containing configuration,
 * optional custom encode/decode functions, separate or shared encoding
 * & decoding tables/dictionaries/schemas, and cached runtime workspaces.
 *
 * Composability / Chaining (ectxparentid):
 * An encoding context may reference a parent encoding context via parent_id.
 * - Only the root stage (parent == NULL) takes the column's SQL type
 *   (anyelement -> bytea); all subsequent child stages operate on
 *   bytea -> bytea.
 * - Encoding runs inside-out (root parent first, down to leaf context) on the
 *   whole datum before Direct TOAST chunking:
 *     compress_zstd(zstd_ctx, encode_avro(avro_ctx, datum))
 * - Decoding runs outside-in (leaf context first, up to root parent) on the
 *   reassembled or sliced bytea:
 *     decode_avro(avro_ctx, decompress_zstd(zstd_ctx, rawtoast))
 *
 * Slicing Support:
 * Decoding functions remain completely slice-agnostic. In Direct TOAST,
 * slice boundaries are recorded in Direct TOAST chunks along with the
 * context_id at chunk/slice boundaries. Slicing reads only the chunks covering
 * the desired byte range and decodes them independently using the
 * associated context.
 *
 * Function Calling Convention (Option B):
 * Both functions take an 'internal' pointer as their first argument, which
 * passes the EncodingContext directly for zero-overhead C-level execution.
 *   enc_fn(internal context, anyelement datum) RETURNS bytea
 *   dec_fn(internal context, bytea encoded_data) RETURNS anyelement
 *
 * Dual / Asymmetric Tables (enctab / dectab):
 * - Compression / Entropy coding (e.g. Huffman coding):
 *     enctab = frequency / encoding tree (symbol -> bit sequence)
 *     dectab = prefix decoding lookup table (bit sequence -> symbol)
 * - Schema-driven serialization (e.g. Apache Avro / Protobuf):
 *     enctab = writer schema / descriptor
 *     dectab = reader schema / projection mapping
 * - Asymmetric encryption / Envelope encryption:
 *     enctab = public key / key encryption key (write-only ingest)
 *     dectab = private key / decryption key (privileged read)
 * - Vector Quantization (e.g. Product Quantization for embeddings):
 *     enctab = trained codebook centroids
 *     dectab = centroid reconstruction table
 *
 * For symmetric algorithms (e.g. raw LZ4 or Zstandard), dectab can be left
 * NULL to share enctab.
 */
typedef struct EncodingContextData
{
	/* Context identification and memory management */
	Oid			context_id;			/* Persistent catalog OID in pg_encoding_context */
	Oid			type_id;			/* Persistent catalog OID in pg_encoding_type */
	Oid			parent_id;			/* Optional parent OID in pg_encoding_context (ectxparentid) */
	Oid			target_typid;		/* Target SQL data type (ectxtargettypid; BYTEAOID if parent_id != 0) */
	char		kind;				/* High-level kind from pg_encoding_type (enctypkind) */
	NameData	algorithm;			/* Algorithm name from pg_encoding_type (enctyptype) */
	struct EncodingContextData *parent; /* Resolved parent context in backend cache (NULL if root) */
	MemoryContext mcxt;				/* MemoryContext owning this context */

	/*
	 * Function OIDs for custom encoding and decoding handlers, cached from
	 * the associated pg_encoding_type. When set to InvalidOid, built-in
	 * algorithms might be used.
	 */
	Oid			enc_fn;				/* Optional: encode_function OID (enctypencfunc) */
	Oid			dec_fn;				/* Optional: decode_function OID (enctypdecfunc) */

	/* Cached function lookup info (initialized on first call if OID valid) */
	FmgrInfo	enc_flinfo;
	FmgrInfo	dec_flinfo;

	/*
	 * Encoding table / dictionary / schema / public key.
	 */
	bytea	   *enctab;				/* Raw encoding table / dictionary */
	int32		enctab_size;		/* Size of encoding table */
	void	   *enc_opaque;			/* Algorithm-specific prepared encoder state */

	/*
	 * Decoding table / dictionary / schema / private key.
	 * If NULL, decoding falls back to enctab for symmetric codecs.
	 */
	bytea	   *dectab;				/* Raw decoding table / dictionary */
	int32		dectab_size;		/* Size of decoding table */
	void	   *dec_opaque;			/* Algorithm-specific prepared decoder state */
} EncodingContextData;

typedef EncodingContextData *EncodingContext;

/* Helper macros: fallback to enctab if dectab is NULL */
#define EncodingContextGetEncTab(ectx) \
	((ectx)->enctab)
#define EncodingContextGetDecTab(ectx) \
	((ectx)->dectab ? (ectx)->dectab : (ectx)->enctab)

/*
 * Backend-local cache API
 *
 * Encoding contexts are cached per-backend in a hash table keyed by
 * context_id. This allows the same context to be reused across multiple
 * tables, columns, and transactions without redundant catalog lookups
 * or repeated dictionary preparation. Invalidating a parent context
 * cascades invalidation to any cached child contexts.
 */
extern void InitEncodingContextCache(void);
extern EncodingContext GetEncodingContext(Oid context_id);
extern void InvalidateEncodingContext(Oid context_id);

/*
 * Runtime invocation wrappers:
 * - EncodingContextEncode traverses the parent chain from root to leaf.
 * - EncodingContextDecode traverses the parent chain from leaf to root.
 */
extern Datum EncodingContextEncode(EncodingContext ectx, Datum val, Oid valtype);
extern Datum EncodingContextDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype);

/* Context lifecycle */
extern EncodingContext CreateEncodingContext(MemoryContext parent_mcxt,
											Oid context_id,
											Oid type_id,
											Oid parent_id,
											Oid target_typid,
											char kind,
											const char *algorithm,
											Oid enc_fn,
											Oid dec_fn);
extern void SetEncodingContextTables(EncodingContext ectx,
									 bytea *enctab,
									 bytea *dectab);
extern void FreeEncodingContext(EncodingContext ectx);

/*
 * Catalog Management C API (pg_encoding_type & pg_encoding_context)
 */
extern Oid	EncodingTypeCreate(const char *name,
							   Oid namespaceId,
							   Oid ownerId,
							   char kind,
							   const char *algorithm,
							   Oid encFuncId,
							   Oid decFuncId);
extern void RemoveEncodingTypeById(Oid type_id);
extern void EncodingTypeRemove(Oid type_id, bool cascade, bool missing_ok);

extern Oid	EncodingContextCreate(const char *name,
								  Oid namespaceId,
								  Oid ownerId,
								  Oid type_id,
								  Oid parent_id,
								  Oid target_typid,
								  bytea *enctab,
								  bytea *dectab,
								  text *label,
								  Datum options);
extern void EncodingContextUpdate(Oid context_id,
								  bytea *enctab, bool update_enctab,
								  bytea *dectab, bool update_dectab,
								  text *label, bool update_label,
								  Datum options, bool update_options);
extern void RemoveEncodingContextById(Oid context_id);
extern void EncodingContextRemove(Oid context_id, bool cascade, bool missing_ok);

extern void ColumnSetEncodingContext(Oid relid, const char *attname, Oid context_id);
extern Oid	GetRootEncodingContextTargetType(Oid context_id);

#endif							/* ENCODING_CONTEXT_H */
