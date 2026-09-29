/*-------------------------------------------------------------------------
 *
 * encoding_context.c
 *	  Backend-local cache, composable runtime execution, catalog management
 *	  C API, and SQL-callable user functions for pg_encoding_type and
 *	  pg_encoding_context.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/common/encoding_context.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/encoding_context.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_encoding_context.h"
#include "catalog/pg_encoding_type.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "common/pg_lzcompress.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/regproc.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Hash table entry for the backend-local EncodingContext cache.
 */
typedef struct EncodingContextCacheEntry
{
	Oid			context_id;		/* hash key - MUST BE FIRST */
	EncodingContext ectx;
	bool		invalidated;	/* mark bit for two-pass cascading eviction */
} EncodingContextCacheEntry;

static MemoryContext EncodingContextMemoryContext = NULL;
static HTAB *EncodingContextHash = NULL;

static EncodingContext GetEncodingContextInternal(Oid context_id, int depth);
static void EncodingContextSyscacheCallback(Datum arg, int cacheid, uint32 hashvalue);
static bool IsValidEncodingKind(char kind);

/* ----------------------------------------------------------------
 *				Backend-Local Cache & Lifecycle Management
 * ----------------------------------------------------------------
 */

/*
 * InitEncodingContextCache
 *		Initializes the backend-local hash table and registers syscache
 *		invalidation callbacks for pg_encoding_context and pg_encoding_type.
 */
void
InitEncodingContextCache(void)
{
	HASHCTL		ctl;

	if (EncodingContextHash != NULL)
		return;

	EncodingContextMemoryContext = AllocSetContextCreate(TopMemoryContext,
														 "EncodingContextCache",
														 ALLOCSET_DEFAULT_SIZES);

	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(Oid);
	ctl.entrysize = sizeof(EncodingContextCacheEntry);
	ctl.hcxt = EncodingContextMemoryContext;

	EncodingContextHash = hash_create("EncodingContextHash",
									  32,
									  &ctl,
									  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	CacheRegisterSyscacheCallback(ENCODINGCONTEXTOID,
								  EncodingContextSyscacheCallback,
								  (Datum) 0);
	CacheRegisterSyscacheCallback(ENCODINGTYPEOID,
								  EncodingContextSyscacheCallback,
								  (Datum) 0);
}

/*
 * CreateEncodingContext
 *		Allocates and initializes an in-memory EncodingContextData in its own
 *		child memory context under parent_mcxt.
 */
EncodingContext
CreateEncodingContext(MemoryContext parent_mcxt,
					  Oid context_id,
					  Oid type_id,
					  Oid parent_id,
					  Oid target_typid,
					  char kind,
					  const char *algorithm,
					  Oid enc_fn,
					  Oid dec_fn)
{
	MemoryContext cxt;
	EncodingContext ectx;

	cxt = AllocSetContextCreate(parent_mcxt,
								"EncodingContextEntry",
								ALLOCSET_SMALL_SIZES);

	ectx = (EncodingContext) MemoryContextAllocZero(cxt, sizeof(EncodingContextData));
	ectx->context_id = context_id;
	ectx->type_id = type_id;
	ectx->parent_id = parent_id;
	ectx->target_typid = target_typid;
	ectx->kind = kind;
	namestrcpy(&ectx->algorithm, algorithm ? algorithm : "");
	ectx->parent = NULL;
	ectx->mcxt = cxt;
	ectx->enc_fn = enc_fn;
	ectx->dec_fn = dec_fn;

	if (OidIsValid(enc_fn))
		fmgr_info_cxt(enc_fn, &ectx->enc_flinfo, cxt);
	if (OidIsValid(dec_fn))
		fmgr_info_cxt(dec_fn, &ectx->dec_flinfo, cxt);

	return ectx;
}

/*
 * SetEncodingContextTables
 *		Copies enctab and dectab into the EncodingContext's owning memory context.
 */
void
SetEncodingContextTables(EncodingContext ectx,
						 bytea *enctab,
						 bytea *dectab)
{
	MemoryContext oldcxt = MemoryContextSwitchTo(ectx->mcxt);

	if (ectx->enctab)
	{
		pfree(ectx->enctab);
		ectx->enctab = NULL;
		ectx->enctab_size = 0;
	}
	if (ectx->dectab)
	{
		pfree(ectx->dectab);
		ectx->dectab = NULL;
		ectx->dectab_size = 0;
	}

	if (enctab != NULL)
	{
		Size		len = VARSIZE_ANY(enctab);

		ectx->enctab = (bytea *) palloc(len);
		memcpy(ectx->enctab, enctab, len);
		ectx->enctab_size = (int32) VARSIZE_ANY_EXHDR(enctab);
	}

	if (dectab != NULL)
	{
		Size		len = VARSIZE_ANY(dectab);

		ectx->dectab = (bytea *) palloc(len);
		memcpy(ectx->dectab, dectab, len);
		ectx->dectab_size = (int32) VARSIZE_ANY_EXHDR(dectab);
	}

	MemoryContextSwitchTo(oldcxt);
}

/*
 * FreeEncodingContext
 *		Releases all memory associated with an EncodingContext.
 */
void
FreeEncodingContext(EncodingContext ectx)
{
	if (ectx == NULL)
		return;

	if (ectx->mcxt != NULL)
		MemoryContextDelete(ectx->mcxt);
}

/*
 * InvalidateEncodingContext
 *		Evicts a specific context_id (or all contexts if InvalidOid) and
 *		cascades eviction to any cached child contexts that depend on it.
 */
void
InvalidateEncodingContext(Oid context_id)
{
	HASH_SEQ_STATUS status;
	EncodingContextCacheEntry *entry;

	if (EncodingContextHash == NULL)
		return;

	/*
	 * Pass 1: Mark all entries to be evicted while all EncodingContext
	 * pointers in parent chains are still valid.
	 */
	hash_seq_init(&status, EncodingContextHash);
	while ((entry = (EncodingContextCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		entry->invalidated = false;

		if (!OidIsValid(context_id) || entry->context_id == context_id)
		{
			entry->invalidated = true;
		}
		else if (entry->ectx != NULL)
		{
			EncodingContext p = entry->ectx->parent;
			int			depth = 0;

			while (p != NULL && depth < MAX_ENCODING_CONTEXT_DEPTH)
			{
				if (p->context_id == context_id)
				{
					entry->invalidated = true;
					break;
				}
				p = p->parent;
				depth++;
			}
		}
	}

	/*
	 * Pass 2: Free marked contexts and remove them from the hash table.
	 */
	hash_seq_init(&status, EncodingContextHash);
	while ((entry = (EncodingContextCacheEntry *) hash_seq_search(&status)) != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (entry->invalidated)
		{
			EncodingContext ectx = entry->ectx;

			hash_search(EncodingContextHash, &entry->context_id, HASH_REMOVE, NULL);
			FreeEncodingContext(ectx);
		}
	}
}

/*
 * EncodingContextSyscacheCallback
 *		Syscache invalidation callback for ENCODINGCONTEXTOID and ENCODINGTYPEOID.
 */
static void
EncodingContextSyscacheCallback(Datum arg, int cacheid, uint32 hashvalue)
{
	if (EncodingContextHash == NULL)
		return;

	/*
	 * Because parent-child chains link cached entries together, flush all
	 * cached entries on any catalog modification to pg_encoding_context or
	 * pg_encoding_type.
	 */
	InvalidateEncodingContext(InvalidOid);
}

/*
 * GetEncodingContext
 *		Looks up an EncodingContext by OID in the backend-local cache, or
 *		loads it (and its parent chain) from the system catalogs on a miss.
 */
EncodingContext
GetEncodingContext(Oid context_id)
{
	if (!OidIsValid(context_id))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding context OID %u", context_id)));

	if (EncodingContextHash == NULL)
		InitEncodingContextCache();

	return GetEncodingContextInternal(context_id, 0);
}

static EncodingContext
GetEncodingContextInternal(Oid context_id, int depth)
{
	EncodingContextCacheEntry *entry;
	bool		found;
	HeapTuple	ctutup;
	HeapTuple	typtup;
	Form_pg_encoding_context ctxForm;
	Form_pg_encoding_type typForm;
	Oid			type_id;
	Oid			parent_id;
	Oid			target_typid;
	char		kind;
	NameData	algorithm;
	Oid			enc_fn;
	Oid			dec_fn;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	Datum		datum;
	bool		isnull;
	EncodingContext parent_ectx = NULL;
	EncodingContext ectx;

	CHECK_FOR_INTERRUPTS();

	if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("encoding context chain depth exceeds maximum (%d)",
						MAX_ENCODING_CONTEXT_DEPTH)));

	entry = (EncodingContextCacheEntry *) hash_search(EncodingContextHash,
													  &context_id,
													  HASH_FIND,
													  &found);
	if (found && entry->ectx != NULL)
		return entry->ectx;

	/* Look up pg_encoding_context tuple */
	ctutup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(context_id));
	if (!HeapTupleIsValid(ctutup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding context with OID %u does not exist", context_id)));

	ctxForm = (Form_pg_encoding_context) GETSTRUCT(ctutup);
	type_id = ctxForm->ectxtypid;
	parent_id = ctxForm->ectxparentid;
	target_typid = ctxForm->ectxtargettypid;

	/* Extract varlena enctab and dectab while holding syscache pin */
	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxenctab, &isnull);
	if (!isnull)
		enctab = DatumGetByteaPCopy(datum);

	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxdectab, &isnull);
	if (!isnull)
		dectab = DatumGetByteaPCopy(datum);

	ReleaseSysCache(ctutup);

	/* Look up pg_encoding_type tuple */
	typtup = SearchSysCache1(ENCODINGTYPEOID, ObjectIdGetDatum(type_id));
	if (!HeapTupleIsValid(typtup))
	{
		if (enctab)
			pfree(enctab);
		if (dectab)
			pfree(dectab);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding type with OID %u does not exist", type_id)));
	}

	typForm = (Form_pg_encoding_type) GETSTRUCT(typtup);
	kind = typForm->enctypkind;
	algorithm = typForm->enctyptype;
	enc_fn = typForm->enctypencfunc;
	dec_fn = typForm->enctypdecfunc;
	ReleaseSysCache(typtup);

	/* Recursively resolve parent context first if chained */
	if (OidIsValid(parent_id))
	{
		if (parent_id == context_id)
		{
			if (enctab)
				pfree(enctab);
			if (dectab)
				pfree(dectab);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("encoding context %u cannot be its own parent", context_id)));
		}
		parent_ectx = GetEncodingContextInternal(parent_id, depth + 1);
	}

	/* Create the EncodingContext in EncodingContextMemoryContext */
	ectx = CreateEncodingContext(EncodingContextMemoryContext,
								 context_id,
								 type_id,
								 parent_id,
								 target_typid,
								 kind,
								 NameStr(algorithm),
								 enc_fn,
								 dec_fn);
	ectx->parent = parent_ectx;
	SetEncodingContextTables(ectx, enctab, dectab);

	if (enctab)
		pfree(enctab);
	if (dectab)
		pfree(dectab);

	/* Insert into backend-local hash table */
	entry = (EncodingContextCacheEntry *) hash_search(EncodingContextHash,
													  &context_id,
													  HASH_ENTER,
													  &found);
	entry->ectx = ectx;
	entry->invalidated = false;

	return ectx;
}

/* ----------------------------------------------------------------
 *					Composable Runtime Execution
 * ----------------------------------------------------------------
 */

/*
 * EncodingContextInvokeSingleEncode
 *		Executes a single stage's encode function with a synthesized FuncExpr
 *		so polymorphic handlers (anyelement) can inspect argument/result types.
 */
static Datum
EncodingContextInvokeSingleEncode(EncodingContext ectx, Datum val, Oid valtype)
{
	LOCAL_FCINFO(fcinfo, 2);
	FuncExpr   *fexpr;
	Node	   *old_expr;
	Datum		result;

	if (!OidIsValid(ectx->enc_fn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("encoding context %u (type \"%s\") has no encode function",
						ectx->context_id, NameStr(ectx->algorithm))));

	/* Validate root target_typid if specified */
	if (ectx->parent == NULL && OidIsValid(ectx->target_typid) &&
		OidIsValid(valtype) && ectx->target_typid != valtype)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("encoding context %u expects type %s, got %s",
						ectx->context_id,
						format_type_be(ectx->target_typid),
						format_type_be(valtype))));

	fexpr = makeFuncExpr(ectx->enc_fn,
						 BYTEAOID,
						 list_make2(makeNullConst(INTERNALOID, -1, InvalidOid),
									makeNullConst(OidIsValid(valtype) ? valtype : BYTEAOID,
												  -1, InvalidOid)),
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	old_expr = ectx->enc_flinfo.fn_expr;
	ectx->enc_flinfo.fn_expr = (Node *) fexpr;

	InitFunctionCallInfoData(*fcinfo, &ectx->enc_flinfo, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = PointerGetDatum(ectx);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = val;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);
	ectx->enc_flinfo.fn_expr = old_expr;

	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("encode function for context %u returned NULL",
						ectx->context_id)));

	return result;
}

/*
 * EncodingContextInvokeSingleDecode
 *		Executes a single stage's decode function with a synthesized FuncExpr.
 */
static Datum
EncodingContextInvokeSingleDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype)
{
	LOCAL_FCINFO(fcinfo, 2);
	FuncExpr   *fexpr;
	Node	   *old_expr;
	Datum		result;

	if (!OidIsValid(ectx->dec_fn))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("encoding context %u (type \"%s\") has no decode function",
						ectx->context_id, NameStr(ectx->algorithm))));

	/* Validate root target_typid if specified */
	if (ectx->parent == NULL && OidIsValid(ectx->target_typid) &&
		OidIsValid(resulttype) && ectx->target_typid != resulttype)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("encoding context %u decodes to type %s, not %s",
						ectx->context_id,
						format_type_be(ectx->target_typid),
						format_type_be(resulttype))));

	fexpr = makeFuncExpr(ectx->dec_fn,
						 OidIsValid(resulttype) ? resulttype : BYTEAOID,
						 list_make2(makeNullConst(INTERNALOID, -1, InvalidOid),
									makeNullConst(BYTEAOID, -1, InvalidOid)),
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	old_expr = ectx->dec_flinfo.fn_expr;
	ectx->dec_flinfo.fn_expr = (Node *) fexpr;

	InitFunctionCallInfoData(*fcinfo, &ectx->dec_flinfo, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = PointerGetDatum(ectx);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = encoded_bytea;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);
	ectx->dec_flinfo.fn_expr = old_expr;

	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("decode function for context %u returned NULL",
						ectx->context_id)));

	return result;
}

/*
 * EncodingContextEncode
 *		Executes the composed encoding pipeline inside-out (from root parent
 *		down to leaf ectx) on the datum.
 *		Example: compress_zstd(zstd_ctx, encode_avro(avro_ctx, datum))
 */
Datum
EncodingContextEncode(EncodingContext ectx, Datum val, Oid valtype)
{
	EncodingContext chain[MAX_ENCODING_CONTEXT_DEPTH];
	int			nstages = 0;
	EncodingContext curr = ectx;
	Datum		cur_datum = val;
	Oid			cur_type = valtype;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context is NULL")));

	while (curr != NULL)
	{
		CHECK_FOR_INTERRUPTS();
		if (nstages >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("encoding context chain depth exceeds maximum (%d)",
							MAX_ENCODING_CONTEXT_DEPTH)));
		chain[nstages++] = curr;
		curr = curr->parent;
	}

	/* Execute from root parent (nstages - 1) down to leaf (0) */
	for (int i = nstages - 1; i >= 0; i--)
	{
		CHECK_FOR_INTERRUPTS();
		cur_datum = EncodingContextInvokeSingleEncode(chain[i], cur_datum, cur_type);
		cur_type = BYTEAOID;
	}

	return cur_datum;
}

/*
 * EncodingContextDecode
 *		Executes the composed decoding pipeline outside-in (from leaf ectx
 *		up to root parent).
 *		Example: decode_avro(avro_ctx, decompress_zstd(zstd_ctx, rawtoast))
 */
Datum
EncodingContextDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype)
{
	EncodingContext curr = ectx;
	Datum		cur_datum = encoded_bytea;
	int			depth = 0;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context is NULL")));

	while (curr != NULL)
	{
		Oid			stage_resulttype;

		CHECK_FOR_INTERRUPTS();
		if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("encoding context chain depth exceeds maximum (%d)",
							MAX_ENCODING_CONTEXT_DEPTH)));

		/* Intermediate stages output BYTEAOID; only the root stage outputs resulttype */
		stage_resulttype = (curr->parent != NULL) ? BYTEAOID : resulttype;
		cur_datum = EncodingContextInvokeSingleDecode(curr, cur_datum, stage_resulttype);

		curr = curr->parent;
		depth++;
	}

	return cur_datum;
}

/* ----------------------------------------------------------------
 *					Catalog Management C API
 * ----------------------------------------------------------------
 */

static bool
IsValidEncodingKind(char kind)
{
	switch (kind)
	{
		case ENCTYPKIND_COMPRESSION:
		case ENCTYPKIND_SERIALIZATION:
		case ENCTYPKIND_ENCRYPTION:
		case ENCTYPKIND_QUANTIZATION:
		case ENCTYPKIND_TOKENIZATION:
		case ENCTYPKIND_INTERNING:
		case ENCTYPKIND_CUSTOM:
			return true;
		default:
			return false;
	}
}

/*
 * GetRootEncodingContextTargetType
 *		Walks the parent chain of context_id to return the root context's
 *		ectxtargettypid, enforcing MAX_ENCODING_CONTEXT_DEPTH against cycles.
 */
Oid
GetRootEncodingContextTargetType(Oid context_id)
{
	Oid			curr_id = context_id;
	Oid			visited[MAX_ENCODING_CONTEXT_DEPTH];
	int			depth = 0;

	while (OidIsValid(curr_id))
	{
		HeapTuple	tup;
		Form_pg_encoding_context ctxForm;
		Oid			parent_id;
		Oid			target_typid;

		CHECK_FOR_INTERRUPTS();

		for (int i = 0; i < depth; i++)
		{
			if (visited[i] == curr_id)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cycle detected in encoding context parent chain at OID %u",
								curr_id)));
		}

		if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("encoding context chain depth exceeds maximum (%d)",
							MAX_ENCODING_CONTEXT_DEPTH)));

		visited[depth++] = curr_id;

		tup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(curr_id));
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("encoding context with OID %u does not exist", curr_id)));

		ctxForm = (Form_pg_encoding_context) GETSTRUCT(tup);
		parent_id = ctxForm->ectxparentid;
		target_typid = ctxForm->ectxtargettypid;
		ReleaseSysCache(tup);

		if (!OidIsValid(parent_id))
			return target_typid;

		curr_id = parent_id;
	}

	return InvalidOid;
}

/*
 * EncodingTypeCreate
 *		Inserts a new entry into pg_encoding_type and registers dependencies.
 */
Oid
EncodingTypeCreate(const char *name,
				   Oid namespaceId,
				   Oid ownerId,
				   char kind,
				   const char *algorithm,
				   Oid encFuncId,
				   Oid decFuncId)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_encoding_type];
	bool		nulls[Natts_pg_encoding_type];
	NameData	typname;
	NameData	algname;
	Oid			typeOid;
	ObjectAddress myself;
	ObjectAddress referenced;

	if (!name || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding type name cannot be empty")));

	if (!IsValidEncodingKind(kind))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid encoding type kind \"%c\"", kind),
				 errhint("Valid kinds are 'c', 's', 'e', 'q', 't', 'd', and 'm'.")));

	if (!algorithm || algorithm[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding algorithm name cannot be empty")));

	if (OidIsValid(encFuncId) && !SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(encFuncId)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("encode function with OID %u does not exist", encFuncId)));

	if (OidIsValid(decFuncId) && !SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(decFuncId)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("decode function with OID %u does not exist", decFuncId)));

	if (SearchSysCacheExists2(ENCODINGTYPENAMENSP,
							  CStringGetDatum(name),
							  ObjectIdGetDatum(namespaceId)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("encoding type \"%s\" already exists in schema \"%s\"",
						name, get_namespace_name(namespaceId))));

	rel = table_open(EncodingTypeRelationId, RowExclusiveLock);

	typeOid = GetNewOidWithIndex(rel, EncodingTypeObjectIdIndexId,
								 Anum_pg_encoding_type_oid);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	namestrcpy(&typname, name);
	namestrcpy(&algname, algorithm);

	values[Anum_pg_encoding_type_oid - 1] = ObjectIdGetDatum(typeOid);
	values[Anum_pg_encoding_type_enctypname - 1] = NameGetDatum(&typname);
	values[Anum_pg_encoding_type_enctypnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_encoding_type_enctypowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_encoding_type_enctypkind - 1] = CharGetDatum(kind);
	values[Anum_pg_encoding_type_enctyptype - 1] = NameGetDatum(&algname);
	values[Anum_pg_encoding_type_enctypencfunc - 1] = ObjectIdGetDatum(encFuncId);
	values[Anum_pg_encoding_type_enctypdecfunc - 1] = ObjectIdGetDatum(decFuncId);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	/* Record dependencies */
	ObjectAddressSet(myself, EncodingTypeRelationId, typeOid);

	/* Dependency on namespace */
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceId);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Dependency on owner */
	recordDependencyOnOwner(EncodingTypeRelationId, typeOid, ownerId);

	/* Dependencies on encode and decode functions */
	if (OidIsValid(encFuncId))
	{
		ObjectAddressSet(referenced, ProcedureRelationId, encFuncId);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}
	if (OidIsValid(decFuncId) && decFuncId != encFuncId)
	{
		ObjectAddressSet(referenced, ProcedureRelationId, decFuncId);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(EncodingTypeRelationId, typeOid, 0);

	CommandCounterIncrement();
	return typeOid;
}

/*
 * RemoveEncodingTypeById
 *		Low-level deletion of a single pg_encoding_type row (called by doDeletion).
 */
void
RemoveEncodingTypeById(Oid type_id)
{
	Relation	rel;
	HeapTuple	tup;

	rel = table_open(EncodingTypeRelationId, RowExclusiveLock);

	tup = SearchSysCache1(ENCODINGTYPEOID, ObjectIdGetDatum(type_id));
	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, RowExclusiveLock);
		elog(ERROR, "cache lookup failed for encoding type %u", type_id);
	}

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);
	table_close(rel, RowExclusiveLock);
}

/*
 * EncodingTypeRemove
 *		Drops a pg_encoding_type entry via performDeletion (enforcing RESTRICT/CASCADE).
 */
void
EncodingTypeRemove(Oid type_id, bool cascade, bool missing_ok)
{
	ObjectAddress object;

	if (!SearchSysCacheExists1(ENCODINGTYPEOID, ObjectIdGetDatum(type_id)))
	{
		if (missing_ok)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding type with OID %u does not exist", type_id)));
	}

	ObjectAddressSet(object, EncodingTypeRelationId, type_id);
	performDeletion(&object, cascade ? DROP_CASCADE : DROP_RESTRICT, 0);
}

/*
 * EncodingContextCreate
 *		Inserts a new entry into pg_encoding_context, validates parent chain
 *		depth and intermediate BYTEAOID type contracts, and records dependencies.
 */
Oid
EncodingContextCreate(const char *name,
					  Oid namespaceId,
					  Oid ownerId,
					  Oid type_id,
					  Oid parent_id,
					  Oid target_typid,
					  bytea *enctab,
					  bytea *dectab,
					  text *label,
					  Datum options)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_encoding_context];
	bool		nulls[Natts_pg_encoding_context];
	NameData	ctxname;
	Oid			contextOid;
	ObjectAddress myself;
	ObjectAddress referenced;

	if (!name || name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context name cannot be empty")));

	if (!SearchSysCacheExists1(ENCODINGTYPEOID, ObjectIdGetDatum(type_id)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding type with OID %u does not exist", type_id)));

	if (SearchSysCacheExists2(ENCODINGCONTEXTNAMENSP,
							  CStringGetDatum(name),
							  ObjectIdGetDatum(namespaceId)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("encoding context \"%s\" already exists in schema \"%s\"",
						name, get_namespace_name(namespaceId))));

	/*
	 * Validate parent chain and intermediate stage type contract:
	 * Only the root stage (parent_id == InvalidOid) takes the column's SQL
	 * type; any chained child stage strictly operates on bytea -> bytea.
	 */
	if (OidIsValid(parent_id))
	{
		Oid			curr = parent_id;
		int			depth = 1;

		while (OidIsValid(curr))
		{
			HeapTuple	ptup;
			Form_pg_encoding_context pform;

			CHECK_FOR_INTERRUPTS();
			if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("encoding context chain depth exceeds maximum (%d)",
								MAX_ENCODING_CONTEXT_DEPTH)));

			ptup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(curr));
			if (!HeapTupleIsValid(ptup))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("parent encoding context with OID %u does not exist",
								curr)));

			pform = (Form_pg_encoding_context) GETSTRUCT(ptup);
			curr = pform->ectxparentid;
			ReleaseSysCache(ptup);
			depth++;
		}

		if (OidIsValid(target_typid) && target_typid != BYTEAOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("chained encoding context with a parent must target bytea (or omit target type)")));

		target_typid = BYTEAOID;
	}
	else if (OidIsValid(target_typid))
	{
		if (!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(target_typid)))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("target type with OID %u does not exist", target_typid)));
	}

	rel = table_open(EncodingContextRelationId, RowExclusiveLock);

	contextOid = GetNewOidWithIndex(rel, EncodingContextObjectIdIndexId,
									Anum_pg_encoding_context_oid);

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	namestrcpy(&ctxname, name);

	values[Anum_pg_encoding_context_oid - 1] = ObjectIdGetDatum(contextOid);
	values[Anum_pg_encoding_context_ectxname - 1] = NameGetDatum(&ctxname);
	values[Anum_pg_encoding_context_ectxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_encoding_context_ectxowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_encoding_context_ectxtypid - 1] = ObjectIdGetDatum(type_id);
	values[Anum_pg_encoding_context_ectxparentid - 1] = ObjectIdGetDatum(parent_id);
	values[Anum_pg_encoding_context_ectxtargettypid - 1] = ObjectIdGetDatum(target_typid);

	if (label != NULL)
		values[Anum_pg_encoding_context_ectxlabel - 1] = PointerGetDatum(label);
	else
		nulls[Anum_pg_encoding_context_ectxlabel - 1] = true;

	if (enctab != NULL)
		values[Anum_pg_encoding_context_ectxenctab - 1] = PointerGetDatum(enctab);
	else
		nulls[Anum_pg_encoding_context_ectxenctab - 1] = true;

	if (dectab != NULL)
		values[Anum_pg_encoding_context_ectxdectab - 1] = PointerGetDatum(dectab);
	else
		nulls[Anum_pg_encoding_context_ectxdectab - 1] = true;

	if (options != (Datum) 0)
		values[Anum_pg_encoding_context_ectxoptions - 1] = options;
	else
		nulls[Anum_pg_encoding_context_ectxoptions - 1] = true;

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	table_close(rel, RowExclusiveLock);

	/* Record dependencies */
	ObjectAddressSet(myself, EncodingContextRelationId, contextOid);

	/* Dependency on namespace */
	ObjectAddressSet(referenced, NamespaceRelationId, namespaceId);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Dependency on owner */
	recordDependencyOnOwner(EncodingContextRelationId, contextOid, ownerId);

	/* Dependency on encoding type */
	ObjectAddressSet(referenced, EncodingTypeRelationId, type_id);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	/* Dependency on parent encoding context */
	if (OidIsValid(parent_id))
	{
		ObjectAddressSet(referenced, EncodingContextRelationId, parent_id);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	/* Dependency on target SQL data type */
	if (OidIsValid(target_typid))
	{
		ObjectAddressSet(referenced, TypeRelationId, target_typid);
		recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	}

	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(EncodingContextRelationId, contextOid, 0);

	CommandCounterIncrement();
	return contextOid;
}

/*
 * EncodingContextUpdate
 *		Updates the variable-length payload fields (enctab, dectab, label,
 *		options) of an existing pg_encoding_context tuple.
 */
void
EncodingContextUpdate(Oid context_id,
					  bytea *enctab, bool update_enctab,
					  bytea *dectab, bool update_dectab,
					  text *label, bool update_label,
					  Datum options, bool update_options)
{
	Relation	rel;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Datum		repl_val[Natts_pg_encoding_context];
	bool		repl_null[Natts_pg_encoding_context];
	bool		repl_repl[Natts_pg_encoding_context];

	rel = table_open(EncodingContextRelationId, RowExclusiveLock);

	oldtup = SearchSysCacheCopy1(ENCODINGCONTEXTOID, ObjectIdGetDatum(context_id));
	if (!HeapTupleIsValid(oldtup))
	{
		table_close(rel, RowExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding context with OID %u does not exist", context_id)));
	}

	MemSet(repl_val, 0, sizeof(repl_val));
	MemSet(repl_null, false, sizeof(repl_null));
	MemSet(repl_repl, false, sizeof(repl_repl));

	if (update_enctab)
	{
		repl_repl[Anum_pg_encoding_context_ectxenctab - 1] = true;
		if (enctab != NULL)
			repl_val[Anum_pg_encoding_context_ectxenctab - 1] = PointerGetDatum(enctab);
		else
			repl_null[Anum_pg_encoding_context_ectxenctab - 1] = true;
	}

	if (update_dectab)
	{
		repl_repl[Anum_pg_encoding_context_ectxdectab - 1] = true;
		if (dectab != NULL)
			repl_val[Anum_pg_encoding_context_ectxdectab - 1] = PointerGetDatum(dectab);
		else
			repl_null[Anum_pg_encoding_context_ectxdectab - 1] = true;
	}

	if (update_label)
	{
		repl_repl[Anum_pg_encoding_context_ectxlabel - 1] = true;
		if (label != NULL)
			repl_val[Anum_pg_encoding_context_ectxlabel - 1] = PointerGetDatum(label);
		else
			repl_null[Anum_pg_encoding_context_ectxlabel - 1] = true;
	}

	if (update_options)
	{
		repl_repl[Anum_pg_encoding_context_ectxoptions - 1] = true;
		if (options != (Datum) 0)
			repl_val[Anum_pg_encoding_context_ectxoptions - 1] = options;
		else
			repl_null[Anum_pg_encoding_context_ectxoptions - 1] = true;
	}

	newtup = heap_modify_tuple(oldtup, RelationGetDescr(rel),
							   repl_val, repl_null, repl_repl);
	CatalogTupleUpdate(rel, &newtup->t_self, newtup);

	heap_freetuple(newtup);
	heap_freetuple(oldtup);
	table_close(rel, RowExclusiveLock);

	InvokeObjectPostAlterHook(EncodingContextRelationId, context_id, 0);
	CommandCounterIncrement();

	InvalidateEncodingContext(context_id);
}

/*
 * RemoveEncodingContextById
 *		Low-level deletion of a single pg_encoding_context row (called by doDeletion).
 */
void
RemoveEncodingContextById(Oid context_id)
{
	Relation	rel;
	HeapTuple	tup;

	rel = table_open(EncodingContextRelationId, RowExclusiveLock);

	tup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(context_id));
	if (!HeapTupleIsValid(tup))
	{
		table_close(rel, RowExclusiveLock);
		elog(ERROR, "cache lookup failed for encoding context %u", context_id);
	}

	CatalogTupleDelete(rel, &tup->t_self);

	ReleaseSysCache(tup);
	table_close(rel, RowExclusiveLock);

	InvalidateEncodingContext(context_id);
}

/*
 * EncodingContextRemove
 *		Drops a pg_encoding_context entry via performDeletion (enforcing RESTRICT/CASCADE).
 */
void
EncodingContextRemove(Oid context_id, bool cascade, bool missing_ok)
{
	ObjectAddress object;

	if (!SearchSysCacheExists1(ENCODINGCONTEXTOID, ObjectIdGetDatum(context_id)))
	{
		if (missing_ok)
			return;
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding context with OID %u does not exist", context_id)));
	}

	ObjectAddressSet(object, EncodingContextRelationId, context_id);
	performDeletion(&object, cascade ? DROP_CASCADE : DROP_RESTRICT, 0);
}

/*
 * DeleteColumnEncodingContextDependency
 *		Removes any existing pg_depend entry from (RelationRelationId, relid, attnum)
 *		referencing EncodingContextRelationId.
 */
static void
DeleteColumnEncodingContextDependency(Oid relid, AttrNumber attnum)
{
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(attnum));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);

		CHECK_FOR_INTERRUPTS();
		if (depForm->refclassid == EncodingContextRelationId)
			CatalogTupleDelete(depRel, &tup->t_self);
	}

	systable_endscan(scan);
	table_close(depRel, RowExclusiveLock);
}

/*
 * ColumnSetEncodingContext
 *		Sets or clears pg_attribute.attencoding_context on a table column
 *		and updates pg_depend accordingly.
 */
void
ColumnSetEncodingContext(Oid relid, const char *attname, Oid context_id)
{
	Relation	rel;
	Relation	attrel;
	HeapTuple	atttup;
	Form_pg_attribute attform;
	AttrNumber	attnum;
	Oid			atttypid;

	if (!attname || attname[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("column name cannot be empty")));

	rel = table_open(relid, AccessExclusiveLock);

	attrel = table_open(AttributeRelationId, RowExclusiveLock);
	atttup = SearchSysCacheCopyAttName(relid, attname);
	if (!HeapTupleIsValid(atttup))
	{
		table_close(attrel, RowExclusiveLock);
		table_close(rel, AccessExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" of relation \"%s\" does not exist",
						attname, RelationGetRelationName(rel))));
	}

	attform = (Form_pg_attribute) GETSTRUCT(atttup);
	attnum = attform->attnum;
	atttypid = attform->atttypid;

	if (attnum <= 0)
	{
		heap_freetuple(atttup);
		table_close(attrel, RowExclusiveLock);
		table_close(rel, AccessExclusiveLock);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot set encoding context on system column \"%s\"",
						attname)));
	}

	if (OidIsValid(context_id))
	{
		HeapTuple	ctxtup;
		Form_pg_encoding_context ctxform;
		char	   *ctxname;
		Oid			root_target_typid;

		ctxtup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(context_id));
		if (!HeapTupleIsValid(ctxtup))
		{
			heap_freetuple(atttup);
			table_close(attrel, RowExclusiveLock);
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("encoding context with OID %u does not exist", context_id)));
		}
		ctxform = (Form_pg_encoding_context) GETSTRUCT(ctxtup);
		ctxname = pstrdup(NameStr(ctxform->ectxname));
		ReleaseSysCache(ctxtup);

		root_target_typid = GetRootEncodingContextTargetType(context_id);
		if (OidIsValid(root_target_typid) && root_target_typid != atttypid)
		{
			heap_freetuple(atttup);
			table_close(attrel, RowExclusiveLock);
			table_close(rel, AccessExclusiveLock);
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("encoding context \"%s\" targets type %s, but column \"%s\" is of type %s",
							ctxname,
							format_type_be(root_target_typid),
							attname,
							format_type_be(atttypid))));
		}
	}

	attform->attencoding_context = context_id;
	CatalogTupleUpdate(attrel, &atttup->t_self, atttup);

	heap_freetuple(atttup);
	table_close(attrel, RowExclusiveLock);

	/* Update pg_depend for this column */
	DeleteColumnEncodingContextDependency(relid, attnum);
	if (OidIsValid(context_id))
	{
		ObjectAddress coladdr;
		ObjectAddress ctxaddr;

		ObjectAddressSubSet(coladdr, RelationRelationId, relid, attnum);
		ObjectAddressSet(ctxaddr, EncodingContextRelationId, context_id);
		recordDependencyOn(&coladdr, &ctxaddr, DEPENDENCY_NORMAL);
	}

	CacheInvalidateRelcache(rel);
	table_close(rel, AccessExclusiveLock);

	CommandCounterIncrement();
}

/* ----------------------------------------------------------------
 *			Built-in Reference & Test Encode/Decode Handlers
 * ----------------------------------------------------------------
 */

/*
 * pg_bytea_dict_encode(internal, anyelement) RETURNS bytea
 *
 * Reference C handler demonstrating:
 * - Option B calling convention (internal EncodingContext pointer)
 * - Asymmetric 256-byte substitution table (when enctab_size == 256 and dectab != NULL)
 *   with cached prepared table in ectx->enc_opaque
 * - Symmetric dictionary XOR transformation (when enctab_size > 0)
 * - 0-byte / NULL enctab identity fallback
 */
Datum
pg_bytea_dict_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const uint8 *src_bytes;
	Size		src_len;
	struct varlena *detoasted = NULL;
	bytea	   *result;
	uint8	   *dst_bytes;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_bytea_dict_encode called with NULL EncodingContext")));

	if (!OidIsValid(valtype))
		valtype = BYTEAOID;

	get_typlenbyvalalign(valtype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		detoasted = pg_detoast_datum_packed(DatumGetPointer(val));
		src_bytes = (const uint8 *) VARDATA_ANY(detoasted);
		src_len = VARSIZE_ANY_EXHDR(detoasted);
	}
	else if (typbyval)
	{
		src_bytes = (const uint8 *) &val;
		src_len = (Size) typlen;
	}
	else
	{
		src_bytes = (const uint8 *) DatumGetPointer(val);
		src_len = (Size) typlen;
	}

	result = (bytea *) palloc(VARHDRSZ + src_len);
	SET_VARSIZE(result, VARHDRSZ + src_len);
	dst_bytes = (uint8 *) VARDATA(result);

	if (ectx->enctab != NULL && ectx->enctab_size == 256 &&
		ectx->dectab != NULL && ectx->dectab_size == 256)
	{
		/* Asymmetric 256-byte lookup table mode; cache in enc_opaque */
		const uint8 *lut;

		if (ectx->enc_opaque == NULL)
		{
			uint8	   *cached_lut = (uint8 *) MemoryContextAlloc(ectx->mcxt, 256);

			memcpy(cached_lut, VARDATA_ANY(ectx->enctab), 256);
			ectx->enc_opaque = cached_lut;
		}
		lut = (const uint8 *) ectx->enc_opaque;

		for (Size i = 0; i < src_len; i++)
			dst_bytes[i] = lut[src_bytes[i]];
	}
	else if (ectx->enctab != NULL && ectx->enctab_size > 0)
	{
		/* Symmetric dictionary XOR transform */
		const uint8 *dict = (const uint8 *) VARDATA_ANY(ectx->enctab);
		int32		dict_len = ectx->enctab_size;

		for (Size i = 0; i < src_len; i++)
			dst_bytes[i] = src_bytes[i] ^ dict[i % dict_len];
	}
	else
	{
		if (src_len > 0)
			memcpy(dst_bytes, src_bytes, src_len);
	}

	if (detoasted != NULL && (Pointer) detoasted != DatumGetPointer(val))
		pfree(detoasted);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_bytea_dict_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_bytea_dict_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const uint8 *src_bytes;
	Size		src_len;
	bytea	   *dectab;
	int32		dectab_len = 0;
	bytea	   *out_varlena;
	uint8	   *dst_bytes;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_bytea_dict_decode called with NULL EncodingContext")));

	if (!OidIsValid(resulttype))
		resulttype = BYTEAOID;

	src_bytes = (const uint8 *) VARDATA_ANY(encoded);
	src_len = VARSIZE_ANY_EXHDR(encoded);

	/* Use EncodingContextGetDecTab which falls back to enctab if dectab is NULL */
	dectab = EncodingContextGetDecTab(ectx);
	if (dectab != NULL)
		dectab_len = (int32) VARSIZE_ANY_EXHDR(dectab);

	out_varlena = (bytea *) palloc(VARHDRSZ + src_len);
	SET_VARSIZE(out_varlena, VARHDRSZ + src_len);
	dst_bytes = (uint8 *) VARDATA(out_varlena);

	if (ectx->enctab != NULL && ectx->enctab_size == 256 &&
		ectx->dectab != NULL && ectx->dectab_size == 256)
	{
		/* Asymmetric 256-byte inverse lookup table mode; cache in dec_opaque */
		const uint8 *lut;

		if (ectx->dec_opaque == NULL)
		{
			uint8	   *cached_lut = (uint8 *) MemoryContextAlloc(ectx->mcxt, 256);

			memcpy(cached_lut, VARDATA_ANY(ectx->dectab), 256);
			ectx->dec_opaque = cached_lut;
		}
		lut = (const uint8 *) ectx->dec_opaque;

		for (Size i = 0; i < src_len; i++)
			dst_bytes[i] = lut[src_bytes[i]];
	}
	else if (dectab != NULL && dectab_len > 0)
	{
		/* Symmetric dictionary XOR inverse */
		const uint8 *dict = (const uint8 *) VARDATA_ANY(dectab);

		for (Size i = 0; i < src_len; i++)
			dst_bytes[i] = src_bytes[i] ^ dict[i % dectab_len];
	}
	else
	{
		if (src_len > 0)
			memcpy(dst_bytes, src_bytes, src_len);
	}

	get_typlenbyvalalign(resulttype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		PG_RETURN_POINTER(out_varlena);
	}
	else if (typbyval)
	{
		Datum		res = 0;

		if (src_len != (Size) typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded payload length %zu does not match type length %d",
							src_len, typlen)));
		memcpy(&res, dst_bytes, typlen);
		pfree(out_varlena);
		return res;
	}
	else
	{
		void	   *ptr = palloc(typlen);

		if (src_len != (Size) typlen)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("decoded payload length %zu does not match type length %d",
							src_len, typlen)));
		memcpy(ptr, dst_bytes, typlen);
		pfree(out_varlena);
		PG_RETURN_POINTER(ptr);
	}
}

/*
 * pg_pglz_context_encode(internal, anyelement) RETURNS bytea
 *
 * Built-in PGLZ compression stage for encoding contexts (can be used standalone
 * or chained as a child compression stage on top of a parent serializer).
 * Format: 1-byte flag (0 = raw, 1 = pglz) + 4-byte raw_size + payload.
 */
Datum
pg_pglz_context_encode(PG_FUNCTION_ARGS)
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
	int32		max_dest_len;
	bytea	   *result;
	char	   *dst_ptr;
	int32		comp_len;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_pglz_context_encode called with NULL EncodingContext")));

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

	max_dest_len = PGLZ_MAX_OUTPUT(src_len);
	result = (bytea *) palloc(VARHDRSZ + 5 + max_dest_len);
	dst_ptr = VARDATA(result);

	comp_len = pglz_compress(src_bytes, src_len, dst_ptr + 5, PGLZ_strategy_always);
	if (comp_len >= 0 && comp_len < src_len)
	{
		dst_ptr[0] = 1;			/* compressed */
		memcpy(dst_ptr + 1, &src_len, sizeof(int32));
		SET_VARSIZE(result, VARHDRSZ + 5 + comp_len);
	}
	else
	{
		dst_ptr[0] = 0;			/* uncompressed fallback */
		memcpy(dst_ptr + 1, &src_len, sizeof(int32));
		if (src_len > 0)
			memcpy(dst_ptr + 5, src_bytes, src_len);
		SET_VARSIZE(result, VARHDRSZ + 5 + src_len);
	}

	if (detoasted != NULL && (Pointer) detoasted != DatumGetPointer(val))
		pfree(detoasted);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_pglz_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_pglz_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	const char *src_ptr;
	int32		encoded_len;
	uint8		flag;
	int32		raw_len;
	bytea	   *out_varlena;
	char	   *dst_bytes;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_pglz_context_decode called with NULL EncodingContext")));

	if (!OidIsValid(resulttype))
		resulttype = BYTEAOID;

	src_ptr = VARDATA_ANY(encoded);
	encoded_len = (int32) VARSIZE_ANY_EXHDR(encoded);

	if (encoded_len < 5)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted pglz context payload (length %d < 5)", encoded_len)));

	flag = (uint8) src_ptr[0];
	memcpy(&raw_len, src_ptr + 1, sizeof(int32));
	if (raw_len < 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("corrupted pglz context raw length %d", raw_len)));

	out_varlena = (bytea *) palloc(VARHDRSZ + raw_len);
	SET_VARSIZE(out_varlena, VARHDRSZ + raw_len);
	dst_bytes = VARDATA(out_varlena);

	if (flag == 1)
	{
		int32		dec_len;

		dec_len = pglz_decompress(src_ptr + 5, encoded_len - 5,
								  dst_bytes, raw_len, true);
		if (dec_len != raw_len)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pglz context decompression failed (expected %d bytes, got %d)",
							raw_len, dec_len)));
	}
	else if (flag == 0)
	{
		if (encoded_len - 5 != raw_len)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("pglz context uncompressed payload size mismatch")));
		if (raw_len > 0)
			memcpy(dst_bytes, src_ptr + 5, raw_len);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("unknown pglz context flag %u", (unsigned) flag)));
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
					 errmsg("decoded payload length %d does not match type length %d",
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
					 errmsg("decoded payload length %d does not match type length %d",
							raw_len, typlen)));
		memcpy(ptr, dst_bytes, typlen);
		pfree(out_varlena);
		PG_RETURN_POINTER(ptr);
	}
}

/* ----------------------------------------------------------------
 *			SQL-Callable Setup, Management & Inspection Functions
 * ----------------------------------------------------------------
 */

/*
 * pg_create_encoding_type(name text, kind "char", algorithm text,
 *                         encfunc regproc, decfunc regproc) RETURNS oid
 */
Datum
pg_create_encoding_type(PG_FUNCTION_ARGS)
{
	text	   *name_txt = PG_GETARG_TEXT_PP(0);
	char		kind = PG_GETARG_CHAR(1);
	text	   *alg_txt = PG_GETARG_TEXT_PP(2);
	Oid			encfunc = PG_GETARG_OID(3);
	Oid			decfunc = PG_GETARG_OID(4);
	List	   *names;
	char	   *objname;
	Oid			namespaceId;
	Oid			oid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to manage encoding types")));

	names = stringToQualifiedNameList(text_to_cstring(name_txt), NULL);
	namespaceId = QualifiedNameGetCreationNamespace(names, &objname);

	oid = EncodingTypeCreate(objname,
							 namespaceId,
							 GetUserId(),
							 kind,
							 text_to_cstring(alg_txt),
							 encfunc,
							 decfunc);

	PG_RETURN_OID(oid);
}

/*
 * pg_drop_encoding_type(type_id oid, cascade bool) RETURNS void
 */
Datum
pg_drop_encoding_type(PG_FUNCTION_ARGS)
{
	Oid			type_id = PG_GETARG_OID(0);
	bool		cascade = PG_GETARG_BOOL(1);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to manage encoding types")));

	EncodingTypeRemove(type_id, cascade, false);
	PG_RETURN_VOID();
}

/*
 * pg_create_encoding_context(name text, type_id oid, parent_id oid,
 *                            target_typid oid, enctab bytea, dectab bytea,
 *                            label text, options text[]) RETURNS oid
 */
Datum
pg_create_encoding_context(PG_FUNCTION_ARGS)
{
	text	   *name_txt;
	Oid			type_id;
	Oid			parent_id = InvalidOid;
	Oid			target_typid = InvalidOid;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	text	   *label = NULL;
	Datum		options = (Datum) 0;
	List	   *names;
	char	   *objname;
	Oid			namespaceId;
	Oid			oid;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to manage encoding contexts")));

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("encoding context name and type_id cannot be NULL")));

	name_txt = PG_GETARG_TEXT_PP(0);
	type_id = PG_GETARG_OID(1);

	if (!PG_ARGISNULL(2))
		parent_id = PG_GETARG_OID(2);
	if (!PG_ARGISNULL(3))
		target_typid = PG_GETARG_OID(3);
	if (!PG_ARGISNULL(4))
		enctab = PG_GETARG_BYTEA_PP(4);
	if (!PG_ARGISNULL(5))
		dectab = PG_GETARG_BYTEA_PP(5);
	if (!PG_ARGISNULL(6))
		label = PG_GETARG_TEXT_PP(6);
	if (!PG_ARGISNULL(7))
		options = PG_GETARG_DATUM(7);

	names = stringToQualifiedNameList(text_to_cstring(name_txt), NULL);
	namespaceId = QualifiedNameGetCreationNamespace(names, &objname);

	oid = EncodingContextCreate(objname,
								namespaceId,
								GetUserId(),
								type_id,
								parent_id,
								target_typid,
								enctab,
								dectab,
								label,
								options);

	PG_RETURN_OID(oid);
}

/*
 * pg_alter_encoding_context(context_id oid, enctab bytea, dectab bytea,
 *                           label text, options text[]) RETURNS void
 */
Datum
pg_alter_encoding_context(PG_FUNCTION_ARGS)
{
	Oid			context_id;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	text	   *label = NULL;
	Datum		options = (Datum) 0;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to manage encoding contexts")));

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("context_id cannot be NULL")));

	context_id = PG_GETARG_OID(0);
	if (!PG_ARGISNULL(1))
		enctab = PG_GETARG_BYTEA_PP(1);
	if (!PG_ARGISNULL(2))
		dectab = PG_GETARG_BYTEA_PP(2);
	if (!PG_ARGISNULL(3))
		label = PG_GETARG_TEXT_PP(3);
	if (!PG_ARGISNULL(4))
		options = PG_GETARG_DATUM(4);

	EncodingContextUpdate(context_id,
						  enctab, true,
						  dectab, true,
						  label, true,
						  options, true);

	PG_RETURN_VOID();
}

/*
 * pg_drop_encoding_context(context_id oid, cascade bool) RETURNS void
 */
Datum
pg_drop_encoding_context(PG_FUNCTION_ARGS)
{
	Oid			context_id = PG_GETARG_OID(0);
	bool		cascade = PG_GETARG_BOOL(1);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to manage encoding contexts")));

	EncodingContextRemove(context_id, cascade, false);
	PG_RETURN_VOID();
}

/*
 * pg_set_column_encoding_context(rel regclass, colname name, context_id oid) RETURNS void
 */
Datum
pg_set_column_encoding_context(PG_FUNCTION_ARGS)
{
	Oid			relid = PG_GETARG_OID(0);
	Name		colname = PG_GETARG_NAME(1);
	Oid			context_id = PG_GETARG_OID(2);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to set column encoding context")));

	ColumnSetEncodingContext(relid, NameStr(*colname), context_id);
	PG_RETURN_VOID();
}

/*
 * pg_encode_datum(context_id oid, val anyelement) RETURNS bytea
 */
Datum
pg_encode_datum(PG_FUNCTION_ARGS)
{
	Oid			context_id = PG_GETARG_OID(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	EncodingContext ectx;
	Datum		res;

	ectx = GetEncodingContext(context_id);
	res = EncodingContextEncode(ectx, val, valtype);

	PG_RETURN_BYTEA_P(DatumGetByteaP(res));
}

/*
 * pg_decode_datum(context_id oid, encoded bytea, target_sample anyelement) RETURNS anyelement
 *
 * Note: marked non-strict so callers can pass NULL::target_type as the 3rd
 * argument to specify the polymorphic return type.
 */
Datum
pg_decode_datum(PG_FUNCTION_ARGS)
{
	Oid			context_id;
	bytea	   *encoded;
	Oid			resulttype;
	EncodingContext ectx;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	context_id = PG_GETARG_OID(0);
	encoded = PG_GETARG_BYTEA_PP(1);
	resulttype = get_fn_expr_argtype(fcinfo->flinfo, 2);
	if (!OidIsValid(resulttype))
		resulttype = get_fn_expr_rettype(fcinfo->flinfo);

	ectx = GetEncodingContext(context_id);
	return EncodingContextDecode(ectx, PointerGetDatum(encoded), resulttype);
}

/*
 * pg_column_raw_encoding(val anyelement) RETURNS bytea
 *
 * Extracts the raw stored payload bytes of a datum without running
 * EncodingContextDecode, allowing dictionary training on parent-encoded
 * chunks (e.g. raw Avro chunks).
 */
Datum
pg_column_raw_encoding(PG_FUNCTION_ARGS)
{
	Datum		val = PG_GETARG_DATUM(0);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 0);
	int16		typlen;
	bool		typbyval;
	char		typalign;
	bytea	   *result;

	get_typlenbyvalalign(valtype, &typlen, &typbyval, &typalign);
	if (typlen == -1)
	{
		struct varlena *v = (struct varlena *) DatumGetPointer(val);
		struct varlena *detoasted = detoast_attr(v);
		Size		len = VARSIZE_ANY_EXHDR(detoasted);

		result = (bytea *) palloc(VARHDRSZ + len);
		SET_VARSIZE(result, VARHDRSZ + len);
		if (len > 0)
			memcpy(VARDATA(result), VARDATA_ANY(detoasted), len);

		if (detoasted != v)
			pfree(detoasted);
	}
	else if (typbyval)
	{
		result = (bytea *) palloc(VARHDRSZ + typlen);
		SET_VARSIZE(result, VARHDRSZ + typlen);
		memcpy(VARDATA(result), &val, typlen);
	}
	else
	{
		result = (bytea *) palloc(VARHDRSZ + typlen);
		SET_VARSIZE(result, VARHDRSZ + typlen);
		memcpy(VARDATA(result), DatumGetPointer(val), typlen);
	}

	PG_RETURN_BYTEA_P(result);
}
