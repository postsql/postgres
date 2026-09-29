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

/* Maximum chain depth for fallback encoding contexts to prevent cycles */
#define MAX_ENCODING_CONTEXT_DEPTH	8

/*
 * Sentinel value indicating that the fallback pointer in
 * EncodingContextData has not yet been lazily resolved.
 * Once resolved, the pointer is either NULL (no fallback)
 * or a valid EncodingContext pointer.
 */
#define ECTX_UNRESOLVED ((struct EncodingContextData *) (uintptr_t) 1)

/*
 * EncodingContextData
 *
 * Represents an in-memory encoding context containing configuration,
 * optional custom encode/decode functions, separate or shared encoding
 * & decoding tables/dictionaries/schemas, and cached runtime workspaces.
 *
 * Context Groups & Variants (ectxgroup + ectxvariant):
 * - Every context has an ectxgroup (Oid) and a 1-byte ectxvariant (uint8, 0..255).
 * - For both standalone contexts and group leaders, ectxgroup equals the
 *   context's own oid (ectxgroup = oid), so pg_attribute.attencoding_context
 *   always stores the ectxgroup / leader OID.
 * - ectxvariant = 0 strictly indicates a standalone context (variants not
 *   supported, and no variant fallback).
 * - ectxvariant IN 1..255 distinguishes contexts within a multi-context group
 *   (ectxgroup). Variants within a group are ordered by ectxvariant ASC:
 *   encoding starts at the lowest ectxvariant ("the encoding") and falls back
 *   to the next higher ectxvariant in the same ectxgroup if encoding fails or
 *   write_ok = false.
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
	NameData	name;				/* Context name in pg_encoding_context (ectxname) */
	Oid			type_id;			/* Persistent catalog OID in pg_encoding_type */
	Oid			target_typid;		/* Target SQL data type (ectxtargettypid; 0 for any) */
	bool		write_ok;			/* Availability flag for encoding/compressing (ectxwriteok) */
	Oid			group_id;			/* Context group OID (ectxgroup; equals context_id for leader/standalone) */
	uint8		variant;			/* 1-byte variant selector (ectxvariant; 0 if standalone) */
	char		kind;				/* High-level kind from pg_encoding_type (enctypkind) */
	NameData	algorithm;			/* Algorithm name from pg_encoding_type (enctyptype) */
	char	   *label;				/* Optional human-readable label (ectxlabel) */
	struct EncodingContextData *fallback; /* Lazily resolved fallback context (ECTX_UNRESOLVED, NULL, or ptr) */
	MemoryContext mcxt;				/* MemoryContext owning this context */

	/*
	 * Function OIDs for custom encoding, decoding, and explain handlers,
	 * cached from the associated pg_encoding_type. When set to InvalidOid,
	 * built-in algorithms might be used.
	 */
	Oid			enc_fn;				/* Optional: encode_function OID (enctypencfunc) */
	Oid			dec_fn;				/* Optional: decode_function OID (enctypdecfunc) */
	Oid			exp_fn;				/* Optional: explain_function OID (enctypexpfunc) */

	/* Cached function lookup info (initialized on first call if OID valid) */
	FmgrInfo	enc_flinfo;
	FmgrInfo	dec_flinfo;
	FmgrInfo	exp_flinfo;

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

	/* Optional key=value options array from pg_encoding_context.ectxoptions */
	struct ArrayType *options;
} EncodingContextData;

typedef EncodingContextData *EncodingContext;

/* Helper macros: fallback to enctab if dectab is NULL */
#define EncodingContextGetEncTab(ectx) \
	((ectx)->enctab)
#define EncodingContextGetDecTab(ectx) \
	((ectx)->dectab ? (ectx)->dectab : (ectx)->enctab)

/*
 * Backend-local cache & resolution API
 *
 * Encoding contexts are cached per-backend in a hash table keyed by
 * context_id. Fallback variants within a context group (ectxgroup) are
 * lazily resolved on demand via EncodingContextGetFallback().
 */
extern PGDLLIMPORT void InitEncodingContextCache(void);
extern PGDLLIMPORT EncodingContext GetEncodingContext(Oid context_id);
extern PGDLLIMPORT EncodingContext EncodingContextGetFallback(EncodingContext ectx);
extern PGDLLIMPORT void InvalidateEncodingContext(Oid context_id);
extern PGDLLIMPORT Oid ResolveEncodingContextOid(const char *name_or_oid, bool missing_ok);
extern PGDLLIMPORT const char *EncodingContextGetOption(EncodingContext ectx, const char *optname);
extern PGDLLIMPORT bool EncodingContextIsWholeDatum(EncodingContext ectx, Oid valtype);

/*
 * Runtime invocation wrappers:
 * - EncodingContextEncode / EncodingContextEncodeWithFallback encodes the
 *   datum using ectx, falling back to the next higher variant in ectxgroup if
 *   write_ok is false or encoding fails.
 * - EncodingContextDecode decodes the encoded bytea datum using ectx.
 */
extern PGDLLIMPORT Datum EncodingContextEncode(EncodingContext ectx, Datum val, Oid valtype);
extern PGDLLIMPORT Datum EncodingContextEncodeWithFallback(EncodingContext ectx,
														   Datum val,
														   Oid valtype,
														   Oid *used_context_id);
extern PGDLLIMPORT Datum EncodingContextDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype);

/* Context lifecycle */
extern PGDLLIMPORT EncodingContext CreateEncodingContext(MemoryContext parent_mcxt,
														 Oid context_id,
														 Oid type_id,
														 Oid target_typid,
														 char kind,
														 const char *algorithm,
														 Oid enc_fn,
														 Oid dec_fn);
extern PGDLLIMPORT void SetEncodingContextTables(EncodingContext ectx,
												 bytea *enctab,
												 bytea *dectab);
extern PGDLLIMPORT void FreeEncodingContext(EncodingContext ectx);

/*
 * Catalog Management C API (pg_encoding_type & pg_encoding_context)
 */
extern PGDLLIMPORT Oid EncodingTypeCreate(const char *name,
										  Oid namespaceId,
										  Oid ownerId,
										  char kind,
										  const char *algorithm,
										  Oid encFuncId,
										  Oid decFuncId,
										  Oid expFuncId);
extern PGDLLIMPORT void RemoveEncodingTypeById(Oid type_id);
extern PGDLLIMPORT void EncodingTypeRemove(Oid type_id, bool cascade, bool missing_ok);

extern PGDLLIMPORT Oid EncodingContextCreate(const char *name,
											 Oid namespaceId,
											 Oid ownerId,
											 Oid type_id,
											 Oid target_typid,
											 bool write_ok,
											 Oid group_id,
											 uint8 variant,
											 bytea *enctab,
											 bytea *dectab,
											 text *label,
											 Datum options);
extern PGDLLIMPORT void EncodingContextUpdate(Oid context_id,
											  bool write_ok, bool update_write_ok,
											  Oid group_id, bool update_group_id,
											  uint8 variant, bool update_variant,
											  bytea *enctab, bool update_enctab,
											  bytea *dectab, bool update_dectab,
											  text *label, bool update_label,
											  Datum options, bool update_options);
extern PGDLLIMPORT void RemoveEncodingContextById(Oid context_id);
extern PGDLLIMPORT void EncodingContextRemove(Oid context_id, bool cascade, bool missing_ok);

extern PGDLLIMPORT void ColumnSetEncodingContext(Oid relid, const char *attname, Oid context_id);
extern PGDLLIMPORT Oid GetRootEncodingContextTargetType(Oid context_id);

#endif							/* ENCODING_CONTEXT_H */
