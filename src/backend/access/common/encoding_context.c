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
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/jsonb.h"
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
static bool ContextDependsOn(EncodingContext ectx, Oid target_id, int depth);

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

static PGFunction
resolve_extension_encoding_func(const char *algorithm, bool is_decode)
{
	const char *shlib = NULL;
	char		funcname[NAMEDATALEN];

	if (algorithm == NULL || algorithm[0] == '\0')
		return NULL;

	if (strcmp(algorithm, "zstd") == 0)
		shlib = "$libdir/pg_zstd";
	else if (strcmp(algorithm, "lz4") == 0)
		shlib = "$libdir/pg_lz4";
	else if (strcmp(algorithm, "avro") == 0)
		shlib = "$libdir/pg_avro";
	else if (strcmp(algorithm, "cascade") == 0 ||
			 strcmp(algorithm, "avro_zstd") == 0)
		shlib = "$libdir/pg_cascade";
	else
		return NULL;

	snprintf(funcname, sizeof(funcname), "pg_%s_context_%s",
			 algorithm, is_decode ? "decode" : "encode");

	return (PGFunction) load_external_function(shlib, funcname, false, NULL);
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
	ectx->target_typid = target_typid;
	ectx->write_ok = true;
	ectx->group_id = context_id;
	ectx->variant = 0;
	ectx->kind = kind;
	namestrcpy(&ectx->algorithm, algorithm ? algorithm : "");
	ectx->fallback = NULL;
	ectx->mcxt = cxt;
	ectx->enc_fn = enc_fn;
	ectx->dec_fn = dec_fn;
	ectx->exp_fn = InvalidOid;

	if (OidIsValid(enc_fn))
		fmgr_info_cxt(enc_fn, &ectx->enc_flinfo, cxt);
	else
	{
		PGFunction	ext_enc = resolve_extension_encoding_func(algorithm, false);

		if (ext_enc != NULL)
		{
			ectx->enc_flinfo.fn_addr = ext_enc;
			ectx->enc_flinfo.fn_oid = InvalidOid;
			ectx->enc_flinfo.fn_nargs = 2;
			ectx->enc_flinfo.fn_strict = true;
			ectx->enc_flinfo.fn_retset = false;
			ectx->enc_flinfo.fn_mcxt = cxt;
		}
	}

	if (OidIsValid(dec_fn))
		fmgr_info_cxt(dec_fn, &ectx->dec_flinfo, cxt);
	else
	{
		PGFunction	ext_dec = resolve_extension_encoding_func(algorithm, true);

		if (ext_dec != NULL)
		{
			ectx->dec_flinfo.fn_addr = ext_dec;
			ectx->dec_flinfo.fn_oid = InvalidOid;
			ectx->dec_flinfo.fn_nargs = 2;
			ectx->dec_flinfo.fn_strict = true;
			ectx->dec_flinfo.fn_retset = false;
			ectx->dec_flinfo.fn_mcxt = cxt;
		}
	}

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
 * ContextDependsOn
 *		Returns true if ectx (or any resolved context in its fallback chain)
 *		matches target_id.
 */
static bool
ContextDependsOn(EncodingContext ectx, Oid target_id, int depth)
{
	if (ectx == NULL || ectx == ECTX_UNRESOLVED || depth >= MAX_ENCODING_CONTEXT_DEPTH)
		return false;

	if (ectx->context_id == target_id)
		return true;

	if (ContextDependsOn(ectx->fallback, target_id, depth + 1))
		return true;

	return false;
}

/*
 * InvalidateEncodingContext
 *		Evicts a specific context_id (or all contexts if InvalidOid) and
 *		resets lazy fallback pointers on cached group variants.
 */
void
InvalidateEncodingContext(Oid context_id)
{
	HASH_SEQ_STATUS status;
	EncodingContextCacheEntry *entry;

	if (EncodingContextHash == NULL)
		return;

	/*
	 * Pass 1: Mark entries to be evicted and reset any cached group fallback
	 * pointers back to ECTX_UNRESOLVED so group membership/variant changes are
	 * re-resolved lazily on next access.
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
			if (ContextDependsOn(entry->ectx->fallback, context_id, 0))
				entry->invalidated = true;
			else if (entry->ectx->variant > 0)
				entry->ectx->fallback = ECTX_UNRESOLVED;
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
	 * Flush all cached entries on any catalog modification to
	 * pg_encoding_context or pg_encoding_type so lazy fallback pointers and
	 * cached dictionaries stay consistent.
	 */
	InvalidateEncodingContext(InvalidOid);
}

/*
 * GetEncodingContext
 *		Looks up an EncodingContext by OID in the backend-local cache, or
 *		loads it from the system catalogs on a miss.
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
	NameData	ctx_name;
	Oid			type_id;
	Oid			target_typid;
	bool		write_ok;
	Oid			group_id;
	uint8		variant;
	char		kind;
	NameData	algorithm;
	Oid			enc_fn;
	Oid			dec_fn;
	Oid			exp_fn;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	char	   *label_str = NULL;
	ArrayType  *options = NULL;
	Datum		datum;
	bool		isnull;
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
	ctx_name = ctxForm->ectxname;
	type_id = ctxForm->ectxtypid;
	target_typid = ctxForm->ectxtargettypid;
	write_ok = ctxForm->ectxwriteok;
	group_id = OidIsValid(ctxForm->ectxgroup) ? ctxForm->ectxgroup : context_id;
	variant = ctxForm->ectxvariant;

	/* Extract varlena enctab, dectab, label, and options while holding syscache pin */
	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxenctab, &isnull);
	if (!isnull)
		enctab = DatumGetByteaPCopy(datum);

	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxdectab, &isnull);
	if (!isnull)
		dectab = DatumGetByteaPCopy(datum);

	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxlabel, &isnull);
	if (!isnull)
		label_str = TextDatumGetCString(datum);

	datum = SysCacheGetAttr(ENCODINGCONTEXTOID, ctutup,
							Anum_pg_encoding_context_ectxoptions, &isnull);
	if (!isnull)
		options = DatumGetArrayTypePCopy(datum);

	ReleaseSysCache(ctutup);

	/* Look up pg_encoding_type tuple */
	typtup = SearchSysCache1(ENCODINGTYPEOID, ObjectIdGetDatum(type_id));
	if (!HeapTupleIsValid(typtup))
	{
		if (enctab)
			pfree(enctab);
		if (dectab)
			pfree(dectab);
		if (label_str)
			pfree(label_str);
		if (options)
			pfree(options);
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding type with OID %u does not exist", type_id)));
	}

	typForm = (Form_pg_encoding_type) GETSTRUCT(typtup);
	kind = typForm->enctypkind;
	algorithm = typForm->enctyptype;
	enc_fn = typForm->enctypencfunc;
	dec_fn = typForm->enctypdecfunc;
	exp_fn = typForm->enctypexpfunc;
	ReleaseSysCache(typtup);

	/* Create the EncodingContext in EncodingContextMemoryContext */
	ectx = CreateEncodingContext(EncodingContextMemoryContext,
								 context_id,
								 type_id,
								 target_typid,
								 kind,
								 NameStr(algorithm),
								 enc_fn,
								 dec_fn);
	ectx->name = ctx_name;
	ectx->write_ok = write_ok;
	ectx->group_id = group_id;
	ectx->variant = variant;
	ectx->fallback = (variant > 0) ? ECTX_UNRESOLVED : NULL;
	ectx->exp_fn = exp_fn;
	if (OidIsValid(exp_fn))
		fmgr_info_cxt(exp_fn, &ectx->exp_flinfo, ectx->mcxt);
	if (label_str != NULL)
	{
		ectx->label = MemoryContextStrdup(ectx->mcxt, label_str);
		pfree(label_str);
	}
	else
		ectx->label = NULL;

	SetEncodingContextTables(ectx, enctab, dectab);

	if (options != NULL)
	{
		Size		opt_len = VARSIZE_ANY(options);
		MemoryContext oldcxt = MemoryContextSwitchTo(ectx->mcxt);

		ectx->options = (ArrayType *) palloc(opt_len);
		memcpy(ectx->options, options, opt_len);
		MemoryContextSwitchTo(oldcxt);
		pfree(options);
	}

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

/*
 * EncodingContextGetFallback
 *		Lazily resolves and returns the next higher variant in ectx->group_id
 *		(ordered by ectxvariant ASC), or NULL if ectx is standalone
 *		(variant == 0) or no higher variant exists in the group.
 */
EncodingContext
EncodingContextGetFallback(EncodingContext ectx)
{
	Relation	rel;
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			best_oid = InvalidOid;
	uint8		best_variant = 0;

	if (ectx == NULL || ectx->variant == 0)
		return NULL;

	if (ectx->fallback != ECTX_UNRESOLVED)
		return ectx->fallback;

	rel = table_open(EncodingContextRelationId, AccessShareLock);
	scan = systable_beginscan(rel, InvalidOid, false, NULL, 0, NULL);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_encoding_context cform = (Form_pg_encoding_context) GETSTRUCT(tup);
		Oid			cand_group = OidIsValid(cform->ectxgroup) ? cform->ectxgroup : cform->oid;

		CHECK_FOR_INTERRUPTS();

		if (cand_group == ectx->group_id &&
			cform->oid != ectx->context_id &&
			cform->ectxvariant > ectx->variant)
		{
			if (!OidIsValid(best_oid) || cform->ectxvariant < best_variant)
			{
				best_oid = cform->oid;
				best_variant = cform->ectxvariant;
			}
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	if (OidIsValid(best_oid))
		ectx->fallback = GetEncodingContext(best_oid);
	else
		ectx->fallback = NULL;

	return ectx->fallback;
}

/*
 * ResolveEncodingContextOid
 *		Resolves an encoding context specification string that may be either:
 *		  1. A numeric OID string (e.g. "16384") matching a row in pg_encoding_context
 *		  2. A (possibly schema-qualified) context name resolved along search_path
 *		Matches the resolution rules used by sqvector.
 */
Oid
ResolveEncodingContextOid(const char *name_or_oid, bool missing_ok)
{
	const char *p;
	List	   *names;
	char	   *schemaname;
	char	   *objname;
	Oid			ectxOid = InvalidOid;

	if (name_or_oid == NULL)
	{
		if (missing_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context name or OID cannot be NULL")));
	}

	p = name_or_oid;
	while (*p != '\0' && isspace((unsigned char) *p))
		p++;

	if (*p == '\0')
	{
		if (missing_ok)
			return InvalidOid;
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context name or OID cannot be empty")));
	}

	/*
	 * 1. Check if the trimmed string is a positive numeric OID.
	 */
	{
		const char * digit_p = p;
		bool		 all_digits = true;

		while (*digit_p != '\0' && !isspace((unsigned char) *digit_p))
		{
			if (!isdigit((unsigned char) *digit_p))
			{
				all_digits = false;
				break;
			}
			digit_p++;
		}
		while (*digit_p != '\0' && isspace((unsigned char) *digit_p))
			digit_p++;

		if (all_digits && *digit_p == '\0')
		{
			char	   *endptr = NULL;
			unsigned long parsed;

			errno = 0;
			parsed = strtoul(p, &endptr, 10);
			if (errno == 0 && endptr != p && parsed > 0 && parsed <= PG_UINT32_MAX)
			{
				Oid			cand_oid = (Oid) parsed;

				if (SearchSysCacheExists1(ENCODINGCONTEXTOID, ObjectIdGetDatum(cand_oid)))
					return cand_oid;
			}
		}
	}

	/*
	 * 2. Parse as a (possibly schema-qualified) identifier and resolve across
	 *    explicit namespace or current search_path.
	 */
	names = stringToQualifiedNameList(p, NULL);
	DeconstructQualifiedName(names, &schemaname, &objname);

	if (schemaname != NULL)
	{
		Oid			namespaceId = LookupExplicitNamespace(schemaname, missing_ok);

		if (OidIsValid(namespaceId))
			ectxOid = GetSysCacheOid2(ENCODINGCONTEXTNAMENSP,
									  Anum_pg_encoding_context_oid,
									  CStringGetDatum(objname),
									  ObjectIdGetDatum(namespaceId));
	}
	else
	{
		List	   *search_path = fetch_search_path(false);
		ListCell   *l;

		foreach(l, search_path)
		{
			Oid			namespaceId = lfirst_oid(l);

			ectxOid = GetSysCacheOid2(ENCODINGCONTEXTNAMENSP,
									  Anum_pg_encoding_context_oid,
									  CStringGetDatum(objname),
									  ObjectIdGetDatum(namespaceId));
			if (OidIsValid(ectxOid))
				break;
		}
		list_free(search_path);
	}

	list_free_deep(names);

	if (!OidIsValid(ectxOid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("encoding context \"%s\" does not exist", name_or_oid)));

	return ectxOid;
}

/*
 * EncodingContextGetOption
 *		Searches ectx->options (a text[] array of "key=value" strings) for
 *		a case-insensitive match on optname. Returns a palloc'd C string in
 *		CurrentMemoryContext if found, or NULL if not present.
 */
const char *
EncodingContextGetOption(EncodingContext ectx, const char *optname)
{
	Datum	   *elems;
	bool	   *nulls;
	int			nelems;
	Size		optlen;
	char	   *found_val = NULL;

	if (ectx == NULL || ectx->options == NULL || optname == NULL)
		return NULL;

	optlen = strlen(optname);
	deconstruct_array_builtin(ectx->options, TEXTOID, &elems, &nulls, &nelems);

	for (int i = 0; i < nelems; i++)
	{
		char	   *kv;

		if (nulls[i])
			continue;

		kv = TextDatumGetCString(elems[i]);
		if (pg_strncasecmp(kv, optname, optlen) == 0 && kv[optlen] == '=')
		{
			if (found_val)
				pfree(found_val);
			found_val = pstrdup(kv + optlen + 1);
		}
		pfree(kv);
	}

	pfree(elems);
	pfree(nulls);
	return found_val;
}

static bool
EncodingContextIsWholeDatumInternal(EncodingContext ectx, Oid valtype, int depth)
{
	EncodingContext curr = ectx;

	check_stack_depth();

	if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
		return false;

	if (valtype == JSONBOID || valtype == JSONOID)
		return true;

	while (curr != NULL)
	{
		const char *wd_opt = EncodingContextGetOption(curr, "whole_datum");

		CHECK_FOR_INTERRUPTS();
		if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
			return false;

		if (wd_opt != NULL)
		{
			bool		is_wd = (pg_strcasecmp(wd_opt, "true") == 0 ||
								 pg_strcasecmp(wd_opt, "on") == 0 ||
								 pg_strcasecmp(wd_opt, "1") == 0 ||
								 pg_strcasecmp(wd_opt, "yes") == 0);

			pfree((void *) wd_opt);
			if (is_wd)
				return true;
		}
		if (curr->kind == ENCTYPKIND_SERIALIZATION ||
			strcmp(NameStr(curr->algorithm), "avro") == 0 ||
			strcmp(NameStr(curr->algorithm), "avro_zstd") == 0)
			return true;
		if (curr->target_typid == JSONBOID || curr->target_typid == JSONOID)
			return true;

		/*
		 * Also check multi-stage cascade contexts configured via WITH (stages = ...)
		 */
		{
			const char *stages_opt = EncodingContextGetOption(curr, "stages");

			if (stages_opt != NULL)
			{
				char	   *raw = pstrdup(stages_opt);
				char	   *p = raw;

				pfree((void *) stages_opt);
				while (*p != '\0')
				{
					char	   *comma = strchr(p, ',');
					char	   *tok = p;
					char	   *endp;
					Oid			st_oid = InvalidOid;

					CHECK_FOR_INTERRUPTS();
					if (comma != NULL)
						*comma = '\0';
					while (*tok == ' ' || *tok == '\t')
						tok++;
					endp = tok + strlen(tok);
					while (endp > tok && (endp[-1] == ' ' || endp[-1] == '\t'))
						*--endp = '\0';

					if (*tok != '\0')
					{
						st_oid = ResolveEncodingContextOid(tok, true);
						if (OidIsValid(st_oid) && st_oid != curr->context_id &&
							SearchSysCacheExists1(ENCODINGCONTEXTOID, ObjectIdGetDatum(st_oid)))
						{
							EncodingContext st_ectx = GetEncodingContext(st_oid);

							if (st_ectx != NULL &&
								EncodingContextIsWholeDatumInternal(st_ectx, InvalidOid, depth + 1))
							{
								pfree(raw);
								return true;
							}
						}
					}

					if (comma == NULL)
						break;
					p = comma + 1;
				}
				pfree(raw);
			}
		}

		curr = EncodingContextGetFallback(curr);
		depth++;
	}
	return false;
}

/*
 * EncodingContextIsWholeDatum
 *		Returns true if the encoding context pipeline requires encoding/decoding
 *		the entire datum as a single logical unit rather than per 2 KB slice
 *		(for example, when any stage is a structured serializer such as Apache
 *		Avro with kind == 's', or when the root target SQL type is jsonb/json).
 */
bool
EncodingContextIsWholeDatum(EncodingContext ectx, Oid valtype)
{
	return EncodingContextIsWholeDatumInternal(ectx, valtype, 0);
}

/* ----------------------------------------------------------------
 *					Runtime Execution & Group Fallback
 * ----------------------------------------------------------------
 */

/*
 * EncodingContextInvokeSingleEncode
 *		Executes a context's encode function with a synthesized FuncExpr
 *		so polymorphic handlers (anyelement) can inspect argument/result types.
 */
static Datum
EncodingContextInvokeSingleEncode(EncodingContext ectx, Datum val, Oid valtype,
								  bool allow_null, bool *isnull_out)
{
	LOCAL_FCINFO(fcinfo, 2);
	FmgrInfo	local_flinfo;
	Oid			actual_valtype;
	List	   *fargs;
	FuncExpr   *fexpr;
	Datum		result;

	if (isnull_out != NULL)
		*isnull_out = false;

	if (!OidIsValid(ectx->enc_fn) && ectx->enc_flinfo.fn_addr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("encoding context %u (type \"%s\") has no encode function",
						ectx->context_id, NameStr(ectx->algorithm))));

	/* Validate target_typid if specified */
	if (OidIsValid(ectx->target_typid) &&
		OidIsValid(valtype) && ectx->target_typid != valtype)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("encoding context %u expects type %s, got %s",
						ectx->context_id,
						format_type_be(ectx->target_typid),
						format_type_be(valtype))));

	actual_valtype = OidIsValid(valtype) ? valtype :
		(OidIsValid(ectx->target_typid) ? ectx->target_typid : BYTEAOID);
	fargs = list_make2(makeNullConst(INTERNALOID, -1, InvalidOid),
					   makeNullConst(actual_valtype, -1, InvalidOid));
	fexpr = makeFuncExpr(ectx->enc_fn,
						 BYTEAOID,
						 fargs,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	local_flinfo = ectx->enc_flinfo;
	local_flinfo.fn_expr = (Node *) fexpr;

	InitFunctionCallInfoData(*fcinfo, &local_flinfo, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = PointerGetDatum(ectx);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = val;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);
	ectx->enc_flinfo.fn_extra = local_flinfo.fn_extra;
	pfree(fexpr);
	list_free_deep(fargs);

	if (fcinfo->isnull)
	{
		if (allow_null && isnull_out != NULL)
		{
			*isnull_out = true;
			return (Datum) 0;
		}
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("encode function for context %u returned NULL",
						ectx->context_id)));
	}

	return result;
}

/*
 * EncodingContextInvokeSingleDecode
 *		Executes a context's decode function with a synthesized FuncExpr.
 */
static Datum
EncodingContextInvokeSingleDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype)
{
	LOCAL_FCINFO(fcinfo, 2);
	FmgrInfo	local_flinfo;
	Oid			actual_resulttype;
	List	   *fargs;
	FuncExpr   *fexpr;
	Datum		result;

	if (!OidIsValid(ectx->dec_fn) && ectx->dec_flinfo.fn_addr == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("encoding context %u (type \"%s\") has no decode function",
						ectx->context_id, NameStr(ectx->algorithm))));

	/* Validate target_typid if specified */
	if (OidIsValid(ectx->target_typid) &&
		OidIsValid(resulttype) && ectx->target_typid != resulttype)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("encoding context %u decodes to type %s, not %s",
						ectx->context_id,
						format_type_be(ectx->target_typid),
						format_type_be(resulttype))));

	actual_resulttype = OidIsValid(resulttype) ? resulttype :
		(OidIsValid(ectx->target_typid) ? ectx->target_typid : BYTEAOID);
	fargs = list_make2(makeNullConst(INTERNALOID, -1, InvalidOid),
					   makeNullConst(BYTEAOID, -1, InvalidOid));
	fexpr = makeFuncExpr(ectx->dec_fn,
						 actual_resulttype,
						 fargs,
						 InvalidOid,
						 InvalidOid,
						 COERCE_EXPLICIT_CALL);

	local_flinfo = ectx->dec_flinfo;
	local_flinfo.fn_expr = (Node *) fexpr;

	InitFunctionCallInfoData(*fcinfo, &local_flinfo, 2, InvalidOid, NULL, NULL);
	fcinfo->args[0].value = PointerGetDatum(ectx);
	fcinfo->args[0].isnull = false;
	fcinfo->args[1].value = encoded_bytea;
	fcinfo->args[1].isnull = false;

	result = FunctionCallInvoke(fcinfo);
	ectx->dec_flinfo.fn_extra = local_flinfo.fn_extra;
	pfree(fexpr);
	list_free_deep(fargs);

	if (fcinfo->isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("decode function for context %u returned NULL",
						ectx->context_id)));

	return result;
}

/*
 * EncodingContextEncodeWithFallback
 *		Executes the encoding handler on the datum.
 *		If write_ok is false or encoding fails (by returning NULL or raising an
 *		error such as value outside allowed domain) and a higher fallback
 *		variant exists in the same context group (ectxgroup), falls back to the
 *		next higher ectxvariant and sets *used_context_id to the context that
 *		actually encoded the datum.
 */
Datum
EncodingContextEncodeWithFallback(EncodingContext ectx,
								  Datum val,
								  Oid valtype,
								  Oid *used_context_id)
{
	EncodingContext curr = ectx;
	int			fallback_depth = 0;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context is NULL")));

	while (curr != NULL)
	{
		EncodingContext next_fallback;

		CHECK_FOR_INTERRUPTS();
		if (fallback_depth >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("encoding context fallback chain depth exceeds maximum (%d)",
							MAX_ENCODING_CONTEXT_DEPTH)));

		next_fallback = EncodingContextGetFallback(curr);

		/* Check availability (write_ok) */
		if (!curr->write_ok)
		{
			if (next_fallback != NULL)
			{
				curr = next_fallback;
				fallback_depth++;
				continue;
			}
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("encoding context \"%s\" is disabled for writing (write_ok = false)",
							NameStr(curr->name))));
		}

		/* Fast path when no fallback variant exists for curr */
		if (next_fallback == NULL)
		{
			Datum		res = EncodingContextInvokeSingleEncode(curr, val, valtype, false, NULL);

			if (used_context_id != NULL)
				*used_context_id = curr->context_id;
			return res;
		}

		/*
		 * A higher fallback variant exists in curr->group_id: attempt encoding
		 * in a temporary child memory context inside PG_TRY so that if the
		 * encoder fails (either by returning NULL or by raising an ERROR when
		 * values are outside the allowed domain), we can cleanly fall back to
		 * next_fallback.
		 */
		{
			MemoryContext caller_mcxt = CurrentMemoryContext;
			MemoryContext try_mcxt;
			Datum		 final_res = (Datum) 0;
			bool		 succeeded = false;

			try_mcxt = AllocSetContextCreate(caller_mcxt,
											 "EncodingContextTryEncode",
											 ALLOCSET_DEFAULT_SIZES);
			PG_TRY();
			{
				Datum		try_res;
				bool		null_returned = false;

				MemoryContextSwitchTo(try_mcxt);
				try_res = EncodingContextInvokeSingleEncode(curr, val, valtype,
															true, &null_returned);
				MemoryContextSwitchTo(caller_mcxt);

				if (!null_returned)
				{
					if (DatumGetPointer(try_res) == DatumGetPointer(val))
					{
						final_res = try_res;
					}
					else
					{
						Size		len = VARSIZE_ANY(DatumGetPointer(try_res));
						bytea	   *copied = (bytea *) palloc(len);

						memcpy(copied, DatumGetPointer(try_res), len);
						final_res = PointerGetDatum(copied);
					}
					succeeded = true;
				}
			}
			PG_CATCH();
			{
				MemoryContextSwitchTo(caller_mcxt);
				FlushErrorState();
				succeeded = false;
			}
			PG_END_TRY();

			MemoryContextDelete(try_mcxt);

			if (succeeded)
			{
				if (used_context_id != NULL)
					*used_context_id = curr->context_id;
				return final_res;
			}
		}

		curr = next_fallback;
		fallback_depth++;
	}

	/* Unreachable */
	elog(ERROR, "encoding context fallback chain exhausted");
	return (Datum) 0;
}

/*
 * EncodingContextEncode
 *		Executes the encoding context on the datum, using group variant
 *		fallback if configured.
 */
Datum
EncodingContextEncode(EncodingContext ectx, Datum val, Oid valtype)
{
	return EncodingContextEncodeWithFallback(ectx, val, valtype, NULL);
}

/*
 * EncodingContextDecode
 *		Executes the decoding context on the encoded bytea datum.
 */
Datum
EncodingContextDecode(EncodingContext ectx, Datum encoded_bytea, Oid resulttype)
{
	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context is NULL")));

	return EncodingContextInvokeSingleDecode(ectx, encoded_bytea, resulttype);
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
 *		Returns the encoding context's ectxtargettypid (or, for a multi-stage
 *		cascade context with a "stages" option, the first stage's target type).
 */
Oid
GetRootEncodingContextTargetType(Oid context_id)
{
	Oid			curr_id = context_id;
	Oid			visited[MAX_ENCODING_CONTEXT_DEPTH];
	int			depth = 0;

	if (!OidIsValid(context_id))
		return InvalidOid;

	while (OidIsValid(curr_id))
	{
		HeapTuple	tup;
		Form_pg_encoding_context ctxForm;
		Oid			target_typid;

		CHECK_FOR_INTERRUPTS();

		for (int i = 0; i < depth; i++)
		{
			if (visited[i] == curr_id)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
						 errmsg("cycle detected in encoding context stages at OID %u",
								curr_id)));
		}

		if (depth >= MAX_ENCODING_CONTEXT_DEPTH)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("encoding context stage depth exceeds maximum (%d)",
							MAX_ENCODING_CONTEXT_DEPTH)));

		visited[depth++] = curr_id;

		tup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(curr_id));
		if (!HeapTupleIsValid(tup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("encoding context with OID %u does not exist", curr_id)));

		ctxForm = (Form_pg_encoding_context) GETSTRUCT(tup);
		target_typid = ctxForm->ectxtargettypid;
		ReleaseSysCache(tup);

		if (OidIsValid(target_typid))
			return target_typid;

		/*
		 * If target_typid is not set directly, check if this is a multi-stage
		 * cascade context with a "stages" option and inspect its first stage.
		 */
		{
			EncodingContext ectx = GetEncodingContext(curr_id);
			const char *stages_opt = EncodingContextGetOption(ectx, "stages");
			Oid			first_stage_oid = InvalidOid;

			if (stages_opt != NULL)
			{
				char	   *raw = pstrdup(stages_opt);
				char	   *comma = strchr(raw, ',');
				char	   *tok = raw;
				char	   *endp;

				pfree((void *) stages_opt);
				if (comma != NULL)
					*comma = '\0';
				while (*tok == ' ' || *tok == '\t')
					tok++;
				endp = tok + strlen(tok);
				while (endp > tok && (endp[-1] == ' ' || endp[-1] == '\t'))
					*--endp = '\0';

				if (*tok != '\0')
					first_stage_oid = ResolveEncodingContextOid(tok, true);
				pfree(raw);
			}

			if (OidIsValid(first_stage_oid))
			{
				curr_id = first_stage_oid;
				continue;
			}
		}

		return InvalidOid;
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
				   Oid decFuncId,
				   Oid expFuncId)
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

	if (OidIsValid(expFuncId) && !SearchSysCacheExists1(PROCOID, ObjectIdGetDatum(expFuncId)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("explain function with OID %u does not exist", expFuncId)));

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
	values[Anum_pg_encoding_type_enctypexpfunc - 1] = ObjectIdGetDatum(expFuncId);

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

	/* Dependencies on encode, decode, and explain functions */
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
	if (OidIsValid(expFuncId) && expFuncId != encFuncId && expFuncId != decFuncId)
	{
		ObjectAddressSet(referenced, ProcedureRelationId, expFuncId);
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
 * ValidateEncodingContextGroupAndVariant
 *		Validates (requested_group_id, variant) for context_id and computes
 *		*resolved_group_id:
 *		- variant == 0 (standalone): requested_group_id must be InvalidOid or
 *		  context_id, no other context may reference context_id as its group,
 *		  and *resolved_group_id = context_id.
 *		- variant > 0 (1..255): if requested_group_id is InvalidOid or
 *		  context_id, context_id is the group leader (*resolved_group_id =
 *		  context_id); otherwise requested_group_id must be an existing group
 *		  leader (ectxvariant > 0 and ectxgroup == requested_group_id) with a
 *		  compatible ectxtargettypid, and (*resolved_group_id, variant) must be
 *		  unique across pg_encoding_context.
 */
static void
ValidateEncodingContextGroupAndVariant(Oid context_id,
									   const char *ctx_name,
									   Oid requested_group_id,
									   uint8 variant,
									   Oid target_typid,
									   Oid *resolved_group_id)
{
	Oid			actual_group_id;
	Relation	scanRel;
	SysScanDesc scan;
	HeapTuple	tup;
	int			group_count = 0;

	if (variant == 0)
	{
		if (OidIsValid(requested_group_id) && requested_group_id != context_id)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("standalone encoding context (variant = 0) cannot belong to another context group")));

		/*
		 * Ensure no other context currently belongs to context_id's group.
		 */
		scanRel = table_open(EncodingContextRelationId, AccessShareLock);
		scan = systable_beginscan(scanRel, InvalidOid, false, NULL, 0, NULL);
		while (HeapTupleIsValid(tup = systable_getnext(scan)))
		{
			Form_pg_encoding_context cform = (Form_pg_encoding_context) GETSTRUCT(tup);
			Oid			cand_group = OidIsValid(cform->ectxgroup) ? cform->ectxgroup : cform->oid;

			CHECK_FOR_INTERRUPTS();
			if (cform->oid != context_id && cand_group == context_id)
			{
				char	   *dep_name = pstrdup(NameStr(cform->ectxname));

				systable_endscan(scan);
				table_close(scanRel, AccessShareLock);
				ereport(ERROR,
						(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
						 errmsg("cannot set variant = 0 on group leader encoding context \"%s\" because context \"%s\" belongs to its group",
								ctx_name, dep_name)));
			}
		}
		systable_endscan(scan);
		table_close(scanRel, AccessShareLock);

		*resolved_group_id = context_id;
		return;
	}

	/* variant > 0 (1..255) */
	actual_group_id = (OidIsValid(requested_group_id) && requested_group_id != context_id)
		? requested_group_id
		: context_id;

	if (actual_group_id != context_id)
	{
		HeapTuple	gtup;
		Form_pg_encoding_context gform;
		NameData	grp_name;
		Oid			grp_leader_group;
		uint8		grp_variant;
		Oid			grp_target_typid;

		gtup = SearchSysCache1(ENCODINGCONTEXTOID, ObjectIdGetDatum(actual_group_id));
		if (!HeapTupleIsValid(gtup))
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("group encoding context with OID %u does not exist",
							actual_group_id)));

		gform = (Form_pg_encoding_context) GETSTRUCT(gtup);
		namestrcpy(&grp_name, NameStr(gform->ectxname));
		grp_leader_group = OidIsValid(gform->ectxgroup) ? gform->ectxgroup : gform->oid;
		grp_variant = gform->ectxvariant;
		grp_target_typid = gform->ectxtargettypid;
		ReleaseSysCache(gtup);

		if (grp_variant == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("encoding context \"%s\" is standalone (variant = 0) and cannot be used as a context group",
							NameStr(grp_name))));

		if (grp_leader_group != actual_group_id)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("encoding context \"%s\" is not a context group leader (its group is OID %u)",
							NameStr(grp_name), grp_leader_group)));

		if (OidIsValid(grp_target_typid) &&
			OidIsValid(target_typid) &&
			grp_target_typid != target_typid)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("encoding context group \"%s\" targets type %s, which does not match target type %s",
							NameStr(grp_name),
							format_type_be(grp_target_typid),
							format_type_be(target_typid))));
	}

	/* Verify (actual_group_id, variant) uniqueness and group size limit */
	scanRel = table_open(EncodingContextRelationId, AccessShareLock);
	scan = systable_beginscan(scanRel, InvalidOid, false, NULL, 0, NULL);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_encoding_context cform = (Form_pg_encoding_context) GETSTRUCT(tup);
		Oid			cand_group = OidIsValid(cform->ectxgroup) ? cform->ectxgroup : cform->oid;

		CHECK_FOR_INTERRUPTS();
		if (cform->oid != context_id && cand_group == actual_group_id)
		{
			if (cform->ectxvariant == variant)
			{
				char	   *dup_name = pstrdup(NameStr(cform->ectxname));

				systable_endscan(scan);
				table_close(scanRel, AccessShareLock);
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("encoding context variant %u already exists in context group (context \"%s\")",
								(unsigned) variant, dup_name)));
			}
			group_count++;
		}
	}
	systable_endscan(scan);
	table_close(scanRel, AccessShareLock);

	if (group_count + 1 > MAX_ENCODING_CONTEXT_DEPTH)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("encoding context group exceeds maximum number of variants (%d)",
						MAX_ENCODING_CONTEXT_DEPTH)));

	*resolved_group_id = actual_group_id;
}

/*
 * EncodingContextCreate
 *		Inserts a new entry into pg_encoding_context, validates context group
 *		and variant rules, and records dependencies.
 */
Oid
EncodingContextCreate(const char *name,
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
					  Datum options)
{
	Relation	rel;
	HeapTuple	tup;
	Datum		values[Natts_pg_encoding_context];
	bool		nulls[Natts_pg_encoding_context];
	NameData	ctxname;
	Oid			contextOid;
	Oid			resolved_group_id = InvalidOid;
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

	if (OidIsValid(target_typid) &&
		!SearchSysCacheExists1(TYPEOID, ObjectIdGetDatum(target_typid)))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("target type with OID %u does not exist", target_typid)));

	rel = table_open(EncodingContextRelationId, RowExclusiveLock);

	contextOid = GetNewOidWithIndex(rel, EncodingContextObjectIdIndexId,
									Anum_pg_encoding_context_oid);

	PG_TRY();
	{
		ValidateEncodingContextGroupAndVariant(contextOid,
											   name,
											   group_id,
											   variant,
											   target_typid,
											   &resolved_group_id);
	}
	PG_CATCH();
	{
		table_close(rel, RowExclusiveLock);
		PG_RE_THROW();
	}
	PG_END_TRY();

	MemSet(values, 0, sizeof(values));
	MemSet(nulls, false, sizeof(nulls));

	namestrcpy(&ctxname, name);

	values[Anum_pg_encoding_context_oid - 1] = ObjectIdGetDatum(contextOid);
	values[Anum_pg_encoding_context_ectxname - 1] = NameGetDatum(&ctxname);
	values[Anum_pg_encoding_context_ectxnamespace - 1] = ObjectIdGetDatum(namespaceId);
	values[Anum_pg_encoding_context_ectxowner - 1] = ObjectIdGetDatum(ownerId);
	values[Anum_pg_encoding_context_ectxtypid - 1] = ObjectIdGetDatum(type_id);
	values[Anum_pg_encoding_context_ectxtargettypid - 1] = ObjectIdGetDatum(target_typid);
	values[Anum_pg_encoding_context_ectxwriteok - 1] = BoolGetDatum(write_ok);
	values[Anum_pg_encoding_context_ectxgroup - 1] = ObjectIdGetDatum(resolved_group_id);
	values[Anum_pg_encoding_context_ectxvariant - 1] = UInt8GetDatum(variant);

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

	/* Dependency on group leader encoding context (when not self) */
	if (OidIsValid(resolved_group_id) && resolved_group_id != contextOid)
	{
		ObjectAddressSet(referenced, EncodingContextRelationId, resolved_group_id);
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
 * DeleteEncodingContextRefDependency
 *		Removes any pg_depend entry from (EncodingContextRelationId, context_id, 0)
 *		referencing (EncodingContextRelationId, ref_context_id, 0).
 */
static void
DeleteEncodingContextRefDependency(Oid context_id, Oid ref_context_id)
{
	Relation	depRel;
	ScanKeyData key[3];
	SysScanDesc scan;
	HeapTuple	tup;

	depRel = table_open(DependRelationId, RowExclusiveLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_classid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(EncodingContextRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_objid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(context_id));
	ScanKeyInit(&key[2],
				Anum_pg_depend_objsubid,
				BTEqualStrategyNumber, F_INT4EQ,
				Int32GetDatum(0));

	scan = systable_beginscan(depRel, DependDependerIndexId, true,
							  NULL, 3, key);
	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend depForm = (Form_pg_depend) GETSTRUCT(tup);

		CHECK_FOR_INTERRUPTS();
		if (depForm->refclassid == EncodingContextRelationId &&
			depForm->refobjid == ref_context_id)
			CatalogTupleDelete(depRel, &tup->t_self);
	}

	systable_endscan(scan);
	table_close(depRel, RowExclusiveLock);
}

/*
 * EncodingContextUpdate
 *		Updates mutable attributes (write_ok, group_id, variant, enctab, dectab,
 *		label, options) of an existing pg_encoding_context tuple.
 */
void
EncodingContextUpdate(Oid context_id,
					  bool write_ok, bool update_write_ok,
					  Oid group_id, bool update_group_id,
					  uint8 variant, bool update_variant,
					  bytea *enctab, bool update_enctab,
					  bytea *dectab, bool update_dectab,
					  text *label, bool update_label,
					  Datum options, bool update_options)
{
	Relation	rel;
	HeapTuple	oldtup;
	HeapTuple	newtup;
	Form_pg_encoding_context oldform;
	NameData	old_name;
	Oid			old_group_id;
	uint8		old_variant;
	Oid			target_typid;
	Oid			new_group_id;
	uint8		new_variant;
	Oid			resolved_group_id;
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

	oldform = (Form_pg_encoding_context) GETSTRUCT(oldtup);
	old_name = oldform->ectxname;
	old_group_id = OidIsValid(oldform->ectxgroup) ? oldform->ectxgroup : context_id;
	old_variant = oldform->ectxvariant;
	target_typid = oldform->ectxtargettypid;

	new_group_id = update_group_id ? group_id : old_group_id;
	new_variant = update_variant ? variant : old_variant;

	/*
	 * If updating variant to 0 without explicitly specifying GROUP, reset
	 * new_group_id to context_id (standalone).
	 */
	if (update_variant && new_variant == 0 && !update_group_id)
		new_group_id = context_id;

	resolved_group_id = old_group_id;
	if (update_group_id || update_variant)
	{
		PG_TRY();
		{
			ValidateEncodingContextGroupAndVariant(context_id,
												   NameStr(old_name),
												   new_group_id,
												   new_variant,
												   target_typid,
												   &resolved_group_id);
		}
		PG_CATCH();
		{
			heap_freetuple(oldtup);
			table_close(rel, RowExclusiveLock);
			PG_RE_THROW();
		}
		PG_END_TRY();
	}

	MemSet(repl_val, 0, sizeof(repl_val));
	MemSet(repl_null, false, sizeof(repl_null));
	MemSet(repl_repl, false, sizeof(repl_repl));

	if (update_write_ok)
	{
		repl_repl[Anum_pg_encoding_context_ectxwriteok - 1] = true;
		repl_val[Anum_pg_encoding_context_ectxwriteok - 1] = BoolGetDatum(write_ok);
	}

	if (update_group_id || update_variant)
	{
		repl_repl[Anum_pg_encoding_context_ectxgroup - 1] = true;
		repl_val[Anum_pg_encoding_context_ectxgroup - 1] = ObjectIdGetDatum(resolved_group_id);
		repl_repl[Anum_pg_encoding_context_ectxvariant - 1] = true;
		repl_val[Anum_pg_encoding_context_ectxvariant - 1] = UInt8GetDatum(new_variant);
	}

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

	if ((update_group_id || update_variant) && old_group_id != resolved_group_id)
	{
		if (OidIsValid(old_group_id) && old_group_id != context_id)
			DeleteEncodingContextRefDependency(context_id, old_group_id);

		if (OidIsValid(resolved_group_id) && resolved_group_id != context_id)
		{
			ObjectAddress myself;
			ObjectAddress referenced;

			ObjectAddressSet(myself, EncodingContextRelationId, context_id);
			ObjectAddressSet(referenced, EncodingContextRelationId, resolved_group_id);
			recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
		}
	}

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
 *			Built-in Reference & Test Encode/Decode/Explain Handlers
 * ----------------------------------------------------------------
 */

static bool
IsRestrictedAlphabetDict(EncodingContext ectx)
{
	if (ectx->enctab != NULL && ectx->dectab != NULL &&
		ectx->enctab_size > 0 && ectx->enctab_size < 256 &&
		ectx->dectab_size == ectx->enctab_size &&
		memcmp(VARDATA_ANY(ectx->enctab),
			   VARDATA_ANY(ectx->dectab),
			   ectx->enctab_size) != 0)
		return true;
	return false;
}

/*
 * pg_bytea_dict_encode(internal, anyelement) RETURNS bytea
 *
 * Reference C handler demonstrating:
 * - Option B calling convention (internal EncodingContext pointer)
 * - Asymmetric 256-byte substitution table (when enctab_size == 256 and dectab != NULL)
 *   with cached prepared table in ectx->enc_opaque
 * - Restricted-alphabet substitution table (when 0 < enctab_size == dectab_size < 256
 *   and enctab != dectab), raising ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE when an input
 *   byte is outside the allowed alphabet in enctab (triggering fallback context)
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
	else if (IsRestrictedAlphabetDict(ectx))
	{
		/*
		 * Restricted-alphabet lookup mode: enctab lists the allowed input
		 * bytes and dectab lists their mapped encoded bytes. Any input byte
		 * not present in enctab raises an out-of-range error so that
		 * EncodingContextEncodeWithFallback falls back to ectx->fallback.
		 */
		const uint8 *allowed;
		const uint8 *mapped;

		if (ectx->enc_opaque == NULL)
		{
			uint8	   *tables = (uint8 *) MemoryContextAllocZero(ectx->mcxt, 512);
			const uint8 *enc_in = (const uint8 *) VARDATA_ANY(ectx->enctab);
			const uint8 *dec_out = (const uint8 *) VARDATA_ANY(ectx->dectab);

			for (int32 k = 0; k < ectx->enctab_size; k++)
			{
				tables[enc_in[k]] = 1;
				tables[256 + enc_in[k]] = dec_out[k];
			}
			ectx->enc_opaque = tables;
		}
		allowed = (const uint8 *) ectx->enc_opaque;
		mapped = allowed + 256;

		for (Size i = 0; i < src_len; i++)
		{
			uint8		b = src_bytes[i];

			if (!allowed[b])
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("input byte 0x%02x is outside the allowed values of encoding context \"%s\"",
								(unsigned) b, NameStr(ectx->name))));
			dst_bytes[i] = mapped[b];
		}
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
	else if (IsRestrictedAlphabetDict(ectx))
	{
		/* Restricted-alphabet inverse lookup: map dectab[k] back to enctab[k] */
		const uint8 *valid;
		const uint8 *unmapped;

		if (ectx->dec_opaque == NULL)
		{
			uint8	   *tables = (uint8 *) MemoryContextAllocZero(ectx->mcxt, 512);
			const uint8 *enc_in = (const uint8 *) VARDATA_ANY(ectx->enctab);
			const uint8 *dec_out = (const uint8 *) VARDATA_ANY(ectx->dectab);

			for (int32 k = 0; k < ectx->enctab_size; k++)
			{
				tables[dec_out[k]] = 1;
				tables[256 + dec_out[k]] = enc_in[k];
			}
			ectx->dec_opaque = tables;
		}
		valid = (const uint8 *) ectx->dec_opaque;
		unmapped = valid + 256;

		for (Size i = 0; i < src_len; i++)
		{
			uint8		b = src_bytes[i];

			if (!valid[b])
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("encoded byte 0x%02x is not in decoding table of encoding context \"%s\"",
								(unsigned) b, NameStr(ectx->name))));
			dst_bytes[i] = unmapped[b];
		}
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

/*
 * ValidateExplainDirection
 *		Validates that the 2nd argument to an explain function is 'encoding'
 *		or 'decoding' (case-insensitive) and returns the normalized string.
 */
static const char *
ValidateExplainDirection(text *dir_txt, bool *is_decoding)
{
	char	   *dir_str = text_to_cstring(dir_txt);

	if (pg_strcasecmp(dir_str, "encoding") == 0)
	{
		*is_decoding = false;
		pfree(dir_str);
		return "encoding";
	}
	if (pg_strcasecmp(dir_str, "decoding") == 0)
	{
		*is_decoding = true;
		pfree(dir_str);
		return "decoding";
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid explain direction \"%s\"", dir_str),
			 errhint("Valid directions are \"encoding\" and \"decoding\".")));
	return NULL;
}

/*
 * pg_bytea_dict_explain(context_oid oid, direction text) RETURNS json
 *
 * Returns a JSON representation of a dictionary / lookup-table encoding
 * context for either 'encoding' or 'decoding'.
 */
Datum
pg_bytea_dict_explain(PG_FUNCTION_ARGS)
{
	Oid			context_id = PG_GETARG_OID(0);
	text	   *dir_txt = PG_GETARG_TEXT_PP(1);
	bool		is_decoding = false;
	const char *direction = ValidateExplainDirection(dir_txt, &is_decoding);
	EncodingContext ectx = GetEncodingContext(context_id);
	EncodingContext fb = EncodingContextGetFallback(ectx);
	Oid			fallback_oid = (fb != NULL) ? fb->context_id : InvalidOid;
	const char *mode;
	const char *active_table;
	int32		table_bytes;
	StringInfoData buf;

	if (ectx->enctab != NULL && ectx->enctab_size == 256 &&
		ectx->dectab != NULL && ectx->dectab_size == 256)
		mode = "asymmetric_lut_256";
	else if (IsRestrictedAlphabetDict(ectx))
		mode = "restricted_lut";
	else if (ectx->enctab != NULL && ectx->enctab_size > 0)
		mode = "symmetric_xor";
	else
		mode = "identity";

	if (!is_decoding)
	{
		active_table = (ectx->enctab != NULL) ? "enctab" : "none";
		table_bytes = ectx->enctab_size;
	}
	else
	{
		if (ectx->dectab != NULL)
		{
			active_table = "dectab";
			table_bytes = ectx->dectab_size;
		}
		else if (ectx->enctab != NULL)
		{
			active_table = "enctab";
			table_bytes = ectx->enctab_size;
		}
		else
		{
			active_table = "none";
			table_bytes = 0;
		}
	}

	initStringInfo(&buf);
	appendStringInfo(&buf, "{\"context_oid\": %u, \"context_name\": ", ectx->context_id);
	escape_json(&buf, NameStr(ectx->name));
	appendStringInfo(&buf,
					 ", \"direction\": \"%s\", \"algorithm\": \"xor_lut\", "
					 "\"mode\": \"%s\", \"write_ok\": %s, \"group_oid\": %u, "
					 "\"variant\": %u, \"fallback_oid\": %u, \"active_table\": \"%s\", "
					 "\"table_bytes\": %d, \"label\": ",
					 direction,
					 mode,
					 ectx->write_ok ? "true" : "false",
					 ectx->group_id,
					 (unsigned) ectx->variant,
					 fallback_oid,
					 active_table,
					 table_bytes);
	if (ectx->label != NULL)
		escape_json(&buf, ectx->label);
	else
		appendStringInfoString(&buf, "null");
	appendStringInfoChar(&buf, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/*
 * pg_pglz_context_explain(context_oid oid, direction text) RETURNS json
 *
 * Returns a JSON representation of a PGLZ compression encoding context
 * for either 'encoding' or 'decoding'.
 */
Datum
pg_pglz_context_explain(PG_FUNCTION_ARGS)
{
	Oid			context_id = PG_GETARG_OID(0);
	text	   *dir_txt = PG_GETARG_TEXT_PP(1);
	bool		is_decoding = false;
	const char *direction = ValidateExplainDirection(dir_txt, &is_decoding);
	EncodingContext ectx = GetEncodingContext(context_id);
	EncodingContext fb = EncodingContextGetFallback(ectx);
	Oid			fallback_oid = (fb != NULL) ? fb->context_id : InvalidOid;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "{\"context_oid\": %u, \"context_name\": ", ectx->context_id);
	escape_json(&buf, NameStr(ectx->name));
	appendStringInfo(&buf,
					 ", \"direction\": \"%s\", \"algorithm\": \"pglz\", "
					 "\"strategy\": \"always\", \"header_bytes\": 5, \"write_ok\": %s, "
					 "\"group_oid\": %u, \"variant\": %u, \"fallback_oid\": %u, \"label\": ",
					 direction,
					 ectx->write_ok ? "true" : "false",
					 ectx->group_id,
					 (unsigned) ectx->variant,
					 fallback_oid);
	if (ectx->label != NULL)
		escape_json(&buf, ectx->label);
	else
		appendStringInfoString(&buf, "null");
	appendStringInfoChar(&buf, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/*
 * pg_explain_encoding_context(context_oid oid, direction text) RETURNS json
 *
 * Invokes the encoding type's registered enctypexpfunc if configured, or
 * builds a default JSON summary of the encoding context for 'encoding' or
 * 'decoding'.
 */
Datum
pg_explain_encoding_context(PG_FUNCTION_ARGS)
{
	Oid			context_id = PG_GETARG_OID(0);
	text	   *dir_txt = PG_GETARG_TEXT_PP(1);
	bool		is_decoding = false;
	const char *direction = ValidateExplainDirection(dir_txt, &is_decoding);
	EncodingContext ectx = GetEncodingContext(context_id);
	EncodingContext fb;
	Oid			fallback_oid;
	StringInfoData buf;

	if (OidIsValid(ectx->exp_fn))
	{
		return FunctionCall2(&ectx->exp_flinfo,
							 ObjectIdGetDatum(context_id),
							 PointerGetDatum(cstring_to_text(direction)));
	}

	fb = EncodingContextGetFallback(ectx);
	fallback_oid = (fb != NULL) ? fb->context_id : InvalidOid;

	initStringInfo(&buf);
	appendStringInfo(&buf, "{\"context_oid\": %u, \"context_name\": ", ectx->context_id);
	escape_json(&buf, NameStr(ectx->name));
	appendStringInfo(&buf,
					 ", \"direction\": \"%s\", \"write_ok\": %s, "
					 "\"group_oid\": %u, \"variant\": %u, \"fallback_oid\": %u, "
					 "\"target_typid\": %u, \"label\": ",
					 direction,
					 ectx->write_ok ? "true" : "false",
					 ectx->group_id,
					 (unsigned) ectx->variant,
					 fallback_oid,
					 ectx->target_typid);
	if (ectx->label != NULL)
		escape_json(&buf, ectx->label);
	else
		appendStringInfoString(&buf, "null");
	appendStringInfoChar(&buf, '}');

	PG_RETURN_TEXT_P(cstring_to_text_with_len(buf.data, buf.len));
}

/* ----------------------------------------------------------------
 *			SQL-Callable Setup, Management & Inspection Functions
 * ----------------------------------------------------------------
 */

/*
 * pg_create_encoding_type(name text, kind "char", algorithm text,
 *                         encfunc regproc, decfunc regproc,
 *                         expfunc regproc DEFAULT 0) RETURNS oid
 */
Datum
pg_create_encoding_type(PG_FUNCTION_ARGS)
{
	text	   *name_txt = PG_GETARG_TEXT_PP(0);
	char		kind = PG_GETARG_CHAR(1);
	text	   *alg_txt = PG_GETARG_TEXT_PP(2);
	Oid			encfunc = PG_GETARG_OID(3);
	Oid			decfunc = PG_GETARG_OID(4);
	Oid			expfunc = (PG_NARGS() > 5 && !PG_ARGISNULL(5)) ? PG_GETARG_OID(5) : InvalidOid;
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
							 decfunc,
							 expfunc);

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
 * pg_create_encoding_context(name text, type_id oid,
 *                            target_typid oid DEFAULT 0,
 *                            enctab bytea DEFAULT NULL,
 *                            dectab bytea DEFAULT NULL,
 *                            label text DEFAULT NULL,
 *                            options text[] DEFAULT NULL,
 *                            write_ok bool DEFAULT true,
 *                            group_id oid DEFAULT 0,
 *                            variant int2 DEFAULT 0) RETURNS oid
 */
Datum
pg_create_encoding_context(PG_FUNCTION_ARGS)
{
	text	   *name_txt;
	Oid			type_id;
	Oid			target_typid = InvalidOid;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	text	   *label = NULL;
	Datum		options = (Datum) 0;
	bool		write_ok = true;
	Oid			group_id = InvalidOid;
	int32		variant_arg = 0;
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
		target_typid = PG_GETARG_OID(2);
	if (!PG_ARGISNULL(3))
		enctab = PG_GETARG_BYTEA_PP(3);
	if (!PG_ARGISNULL(4))
		dectab = PG_GETARG_BYTEA_PP(4);
	if (!PG_ARGISNULL(5))
		label = PG_GETARG_TEXT_PP(5);
	if (!PG_ARGISNULL(6))
		options = PG_GETARG_DATUM(6);
	if (PG_NARGS() > 7 && !PG_ARGISNULL(7))
		write_ok = PG_GETARG_BOOL(7);
	if (PG_NARGS() > 8 && !PG_ARGISNULL(8))
		group_id = PG_GETARG_OID(8);
	if (PG_NARGS() > 9 && !PG_ARGISNULL(9))
		variant_arg = PG_GETARG_INT32(9);

	if (variant_arg < 0 || variant_arg > 255)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("encoding context variant %d is out of range (must be between 0 and 255)",
						(int) variant_arg)));

	names = stringToQualifiedNameList(text_to_cstring(name_txt), NULL);
	namespaceId = QualifiedNameGetCreationNamespace(names, &objname);

	oid = EncodingContextCreate(objname,
								namespaceId,
								GetUserId(),
								type_id,
								target_typid,
								write_ok,
								group_id,
								(uint8) variant_arg,
								enctab,
								dectab,
								label,
								options);

	PG_RETURN_OID(oid);
}

/*
 * pg_alter_encoding_context(context_id oid, enctab bytea, dectab bytea,
 *                           label text, options text[],
 *                           write_ok bool DEFAULT NULL,
 *                           group_id oid DEFAULT NULL,
 *                           variant int2 DEFAULT NULL) RETURNS void
 */
Datum
pg_alter_encoding_context(PG_FUNCTION_ARGS)
{
	Oid			context_id;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	text	   *label = NULL;
	Datum		options = (Datum) 0;
	bool		write_ok = true;
	bool		update_write_ok = false;
	Oid			group_id = InvalidOid;
	bool		update_group_id = false;
	uint8		variant = 0;
	bool		update_variant = false;

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
	if (PG_NARGS() > 5 && !PG_ARGISNULL(5))
	{
		write_ok = PG_GETARG_BOOL(5);
		update_write_ok = true;
	}
	if (PG_NARGS() > 6 && !PG_ARGISNULL(6))
	{
		group_id = PG_GETARG_OID(6);
		update_group_id = true;
	}
	if (PG_NARGS() > 7 && !PG_ARGISNULL(7))
	{
		int32		variant_arg = PG_GETARG_INT32(7);

		if (variant_arg < 0 || variant_arg > 255)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("encoding context variant %d is out of range (must be between 0 and 255)",
							(int) variant_arg)));
		variant = (uint8) variant_arg;
		update_variant = true;
	}

	EncodingContextUpdate(context_id,
						  write_ok, update_write_ok,
						  group_id, update_group_id,
						  variant, update_variant,
						  enctab, true,
						  dectab, true,
						  label, true,
						  options, true);

	PG_RETURN_VOID();
}

/*
 * pg_resolve_encoding_context_oid(name_or_oid text, missing_ok bool DEFAULT false) RETURNS oid
 */
Datum
pg_resolve_encoding_context_oid(PG_FUNCTION_ARGS)
{
	text	   *spec_txt;
	bool		missing_ok = false;
	char	   *spec_str;
	Oid			oid;

	if (PG_ARGISNULL(0))
	{
		if (PG_NARGS() > 1 && !PG_ARGISNULL(1) && PG_GETARG_BOOL(1))
			PG_RETURN_OID(InvalidOid);
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("encoding context name or OID cannot be NULL")));
	}

	spec_txt = PG_GETARG_TEXT_PP(0);
	if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
		missing_ok = PG_GETARG_BOOL(1);

	spec_str = text_to_cstring(spec_txt);
	oid = ResolveEncodingContextOid(spec_str, missing_ok);
	pfree(spec_str);

	PG_RETURN_OID(oid);
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
