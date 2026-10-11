/*-------------------------------------------------------------------------
 *
 * encodingcmds.c
 *	  SQL DDL utility command handlers for pg_encoding_type and
 *	  pg_encoding_context (CREATE/ALTER ENCODING TYPE/CONTEXT).
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/encodingcmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/encoding_context.h"
#include "catalog/namespace.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_encoding_context.h"
#include "catalog/pg_encoding_type.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "parser/parse_func.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

static bytea *
string_to_context_bytea(const char *str)
{
	const char *p = str;

	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
		p++;

	if (*p == '{' || *p == '[')
		return (bytea *) cstring_to_text(str);

	return DatumGetByteaPP(DirectFunctionCall1(byteain, CStringGetDatum(str)));
}

/*
 * resolve_encoding_handler_func
 *		Resolve a 2-argument encode or decode handler function by name.
 */
static Oid
resolve_encoding_handler_func(DefElem *defel, bool is_encode)
{
	List	   *funcName = defGetQualifiedName(defel);
	FuncCandidateList clist;
	int			fgc_flags;
	Oid			matchedOid = InvalidOid;
	int			nmatches = 0;

	if (list_length(funcName) == 1)
	{
		const char *str = strVal(linitial(funcName));

		if (strcmp(str, "-") == 0 || strcmp(str, "0") == 0 ||
			pg_strcasecmp(str, "none") == 0)
			return InvalidOid;
	}

	clist = FuncnameGetCandidates(funcName, 2, NIL, false, false, false, false,
								  &fgc_flags);
	if (clist == NULL)
	{
		/*
		 * Also check if any function with that name exists with wrong arity
		 * so EncodingTypeCreate can emit its arity diagnostic, or throw
		 * undefined_function if none exists at all.
		 */
		clist = FuncnameGetCandidates(funcName, -1, NIL, false, false, false, false,
									  &fgc_flags);
		if (clist != NULL && clist->next == NULL)
			return clist->oid;

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("function %s does not exist",
						NameListToString(funcName))));
	}

	for (FuncCandidateList c = clist; c != NULL; c = c->next)
	{
		if (c->args[0] == INTERNALOID &&
			(is_encode || c->args[1] == BYTEAOID))
		{
			matchedOid = c->oid;
			nmatches++;
		}
	}

	if (nmatches > 1)
		ereport(ERROR,
				(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
				 errmsg("more than one function named \"%s\"",
						NameListToString(funcName))));

	if (nmatches == 1)
		return matchedOid;

	/*
	 * Fall back to the first candidate so EncodingTypeCreate reports the
	 * exact signature mismatch.
	 */
	return clist->oid;
}

/*
 * deflist_to_options_array
 *		Convert a list of DefElem options into a text[] array of "key=value"
 *		entries, or (Datum) 0 if the list is empty.
 */
static Datum
deflist_to_options_array(List *deflist)
{
	ArrayBuildState *astate = NULL;
	ListCell   *lc;

	if (deflist == NIL)
		return (Datum) 0;

	foreach(lc, deflist)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);
		const char *val = defel->arg ? defGetString(defel) : "";
		char	   *kv = psprintf("%s=%s", defel->defname, val);

		astate = accumArrayResult(astate,
								  PointerGetDatum(cstring_to_text(kv)),
								  false, TEXTOID, CurrentMemoryContext);
		pfree(kv);
	}

	return makeArrayResult(astate, CurrentMemoryContext);
}

/*
 * DefineEncodingType
 *		Executes CREATE ENCODING TYPE.
 */
ObjectAddress
DefineEncodingType(ParseState *pstate, List *names, List *parameters,
				   bool if_not_exists)
{
	char	   *enctypname;
	Oid			namespaceId;
	DefElem    *kindEl = NULL;
	DefElem    *typeEl = NULL;
	DefElem    *encfuncEl = NULL;
	DefElem    *decfuncEl = NULL;
	char		kind = '\0';
	char	   *algorithm = NULL;
	Oid			encFuncId = InvalidOid;
	Oid			decFuncId = InvalidOid;
	Oid			typeOid;
	ObjectAddress address;
	ListCell   *pl;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create encoding types")));

	namespaceId = QualifiedNameGetCreationNamespace(names, &enctypname);

	if (if_not_exists &&
		SearchSysCacheExists2(ENCODINGTYPENAMENSP,
							  CStringGetDatum(enctypname),
							  ObjectIdGetDatum(namespaceId)))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("encoding type \"%s\" already exists, skipping",
						enctypname)));
		return InvalidObjectAddress;
	}

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "kind") == 0)
		{
			char	   *kstr;

			if (kindEl != NULL)
				errorConflictingDefElem(defel, pstate);
			kindEl = defel;
			kstr = defGetString(defel);
			if (strlen(kstr) != 1)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("encoding type kind must be a single character")));
			kind = kstr[0];
		}
		else if (strcmp(defel->defname, "type") == 0 ||
				 strcmp(defel->defname, "algorithm") == 0)
		{
			if (typeEl != NULL)
				errorConflictingDefElem(defel, pstate);
			typeEl = defel;
			algorithm = defGetString(defel);
		}
		else if (strcmp(defel->defname, "encfunc") == 0)
		{
			if (encfuncEl != NULL)
				errorConflictingDefElem(defel, pstate);
			encfuncEl = defel;
			encFuncId = resolve_encoding_handler_func(defel, true);
		}
		else if (strcmp(defel->defname, "decfunc") == 0)
		{
			if (decfuncEl != NULL)
				errorConflictingDefElem(defel, pstate);
			decfuncEl = defel;
			decFuncId = resolve_encoding_handler_func(defel, false);
		}
		else
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized encoding type parameter \"%s\"",
							defel->defname),
					 parser_errposition(pstate, defel->location)));
		}
	}

	if (kindEl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding type kind is required")));

	if (typeEl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding type algorithm (TYPE) is required")));

	/*
	 * If neither ENCFUNC nor DECFUNC was specified, resolve built-in or
	 * extension-installed default handler functions for the algorithm
	 * (pg_<algorithm>_context_encode / pg_<algorithm>_context_decode).
	 */
	if (encfuncEl == NULL && decfuncEl == NULL && algorithm != NULL)
	{
		if (pg_strcasecmp(algorithm, "pglz") == 0)
		{
			encFuncId = F_PG_PGLZ_CONTEXT_ENCODE;
			decFuncId = F_PG_PGLZ_CONTEXT_DECODE;
		}
		else
		{
			char	   *alg_lower = pstrdup(algorithm);
			char	   *enc_name;
			char	   *dec_name;
			List	   *fn_names;
			int			fgc_flags;
			FuncCandidateList clist;

			for (char *p = alg_lower; *p != '\0'; p++)
				*p = pg_ascii_tolower((unsigned char) *p);

			enc_name = psprintf("pg_%s_context_encode", alg_lower);
			fn_names = list_make1(makeString(enc_name));
			clist = FuncnameGetCandidates(fn_names, 2, NIL, false, false, false, true,
										  &fgc_flags);
			for (FuncCandidateList c = clist; c != NULL; c = c->next)
			{
				if (c->args[0] == INTERNALOID)
				{
					encFuncId = c->oid;
					break;
				}
			}
			list_free_deep(fn_names);

			dec_name = psprintf("pg_%s_context_decode", alg_lower);
			fn_names = list_make1(makeString(dec_name));
			clist = FuncnameGetCandidates(fn_names, 2, NIL, false, false, false, true,
										  &fgc_flags);
			for (FuncCandidateList c = clist; c != NULL; c = c->next)
			{
				if (c->args[0] == INTERNALOID && c->args[1] == BYTEAOID)
				{
					decFuncId = c->oid;
					break;
				}
			}
			list_free_deep(fn_names);
			pfree(alg_lower);
		}
	}

	typeOid = EncodingTypeCreate(enctypname,
								 namespaceId,
								 GetUserId(),
								 kind,
								 algorithm,
								 encFuncId,
								 decFuncId);

	ObjectAddressSet(address, EncodingTypeRelationId, typeOid);
	return address;
}

/*
 * DefineEncodingContext
 *		Executes CREATE ENCODING CONTEXT.
 */
ObjectAddress
DefineEncodingContext(ParseState *pstate, List *names, List *parameters,
					  bool if_not_exists)
{
	char	   *ectxname;
	Oid			namespaceId;
	DefElem    *typeEl = NULL;
	DefElem    *parentEl = NULL;
	DefElem    *forTypeEl = NULL;
	DefElem    *enctabEl = NULL;
	DefElem    *dectabEl = NULL;
	DefElem    *dictEl = NULL;
	DefElem    *labelEl = NULL;
	Oid			typeId = InvalidOid;
	Oid			parentId = InvalidOid;
	Oid			targetTypId = InvalidOid;
	bytea	   *enctab = NULL;
	bytea	   *dectab = NULL;
	text	   *label = NULL;
	List	   *extra_options = NIL;
	Datum		options_datum;
	Oid			ctxOid;
	ObjectAddress address;
	ListCell   *pl;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create encoding contexts")));

	namespaceId = QualifiedNameGetCreationNamespace(names, &ectxname);

	if (if_not_exists &&
		SearchSysCacheExists2(ENCODINGCONTEXTNAMENSP,
							  CStringGetDatum(ectxname),
							  ObjectIdGetDatum(namespaceId)))
	{
		ereport(NOTICE,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("encoding context \"%s\" already exists, skipping",
						ectxname)));
		return InvalidObjectAddress;
	}

	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "type") == 0)
		{
			if (typeEl != NULL)
				errorConflictingDefElem(defel, pstate);
			typeEl = defel;
			typeId = get_encoding_type_oid(defGetQualifiedName(defel), false);
		}
		else if (strcmp(defel->defname, "parent") == 0)
		{
			if (parentEl != NULL)
				errorConflictingDefElem(defel, pstate);
			parentEl = defel;
			parentId = get_encoding_context_oid(defGetQualifiedName(defel), false);
		}
		else if (strcmp(defel->defname, "for_type") == 0 ||
				 strcmp(defel->defname, "target_type") == 0)
		{
			if (forTypeEl != NULL)
				errorConflictingDefElem(defel, pstate);
			forTypeEl = defel;
			targetTypId = typenameTypeId(pstate, defGetTypeName(defel));
		}
		else if (strcmp(defel->defname, "enctab") == 0)
		{
			char	   *str;

			if (enctabEl != NULL)
				errorConflictingDefElem(defel, pstate);
			enctabEl = defel;
			str = defGetString(defel);
			enctab = string_to_context_bytea(str);
		}
		else if (strcmp(defel->defname, "dectab") == 0)
		{
			char	   *str;

			if (dectabEl != NULL)
				errorConflictingDefElem(defel, pstate);
			dectabEl = defel;
			str = defGetString(defel);
			dectab = string_to_context_bytea(str);
		}
		else if (strcmp(defel->defname, "dictionary") == 0)
		{
			char	   *str = defGetString(defel);

			dictEl = defel;
			if (enctabEl == NULL)
				enctab = string_to_context_bytea(str);
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "schema") == 0 ||
				 strcmp(defel->defname, "writer_schema") == 0)
		{
			char	   *str = defGetString(defel);

			if (enctabEl == NULL && dictEl == NULL && enctab == NULL)
				enctab = (bytea *) cstring_to_text(str);
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "reader_schema") == 0)
		{
			char	   *str = defGetString(defel);

			if (dectabEl == NULL && dectab == NULL)
				dectab = (bytea *) cstring_to_text(str);
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "label") == 0)
		{
			if (labelEl != NULL)
				errorConflictingDefElem(defel, pstate);
			labelEl = defel;
			label = cstring_to_text(defGetString(defel));
		}
		else
		{
			extra_options = lappend(extra_options, defel);
		}
	}

	if (typeEl == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("encoding type is required")));

	options_datum = deflist_to_options_array(extra_options);

	ctxOid = EncodingContextCreate(ectxname,
								   namespaceId,
								   GetUserId(),
								   typeId,
								   parentId,
								   targetTypId,
								   enctab,
								   dectab,
								   label,
								   options_datum);

	ObjectAddressSet(address, EncodingContextRelationId, ctxOid);
	return address;
}

/*
 * AlterEncodingContext
 *		Executes ALTER ENCODING CONTEXT name [WITH | SET] ( ... ).
 */
ObjectAddress
AlterEncodingContext(AlterEncodingContextStmt *stmt)
{
	Oid			contextId;
	DefElem    *enctabEl = NULL;
	DefElem    *dectabEl = NULL;
	DefElem    *dictEl = NULL;
	DefElem    *labelEl = NULL;
	bytea	   *enctab = NULL;
	bool		update_enctab = false;
	bytea	   *dectab = NULL;
	bool		update_dectab = false;
	text	   *label = NULL;
	bool		update_label = false;
	List	   *extra_options = NIL;
	Datum		options_datum = (Datum) 0;
	bool		update_options = false;
	ObjectAddress address;
	ListCell   *pl;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to alter encoding contexts")));

	contextId = get_encoding_context_oid(stmt->contextname, false);

	if (!object_ownercheck(EncodingContextRelationId, contextId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_ENCODING_CONTEXT,
					   NameListToString(stmt->contextname));

	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "enctab") == 0)
		{
			if (enctabEl != NULL)
				errorConflictingDefElem(defel, NULL);
			enctabEl = defel;
			update_enctab = true;
			enctab = NULL;
			if (defel->arg != NULL)
			{
				char	   *str = defGetString(defel);

				if (pg_strcasecmp(str, "null") != 0 &&
					pg_strcasecmp(str, "none") != 0)
					enctab = string_to_context_bytea(str);
			}
		}
		else if (strcmp(defel->defname, "dectab") == 0)
		{
			if (dectabEl != NULL)
				errorConflictingDefElem(defel, NULL);
			dectabEl = defel;
			update_dectab = true;
			dectab = NULL;
			if (defel->arg != NULL)
			{
				char	   *str = defGetString(defel);

				if (pg_strcasecmp(str, "null") != 0 &&
					pg_strcasecmp(str, "none") != 0)
					dectab = string_to_context_bytea(str);
			}
		}
		else if (strcmp(defel->defname, "dictionary") == 0)
		{
			dictEl = defel;
			if (defel->arg != NULL && enctabEl == NULL)
			{
				char	   *str = defGetString(defel);

				update_enctab = true;
				enctab = NULL;
				if (pg_strcasecmp(str, "null") != 0 &&
					pg_strcasecmp(str, "none") != 0)
					enctab = string_to_context_bytea(str);
			}
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "schema") == 0 ||
				 strcmp(defel->defname, "writer_schema") == 0)
		{
			if (defel->arg != NULL)
			{
				char	   *str = defGetString(defel);

				if (enctabEl == NULL && dictEl == NULL)
				{
					update_enctab = true;
					enctab = NULL;
					if (pg_strcasecmp(str, "null") != 0 &&
						pg_strcasecmp(str, "none") != 0)
						enctab = (bytea *) cstring_to_text(str);
				}
			}
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "reader_schema") == 0)
		{
			if (defel->arg != NULL)
			{
				char	   *str = defGetString(defel);

				if (dectabEl == NULL)
				{
					update_dectab = true;
					dectab = NULL;
					if (pg_strcasecmp(str, "null") != 0 &&
						pg_strcasecmp(str, "none") != 0)
						dectab = (bytea *) cstring_to_text(str);
				}
			}
			extra_options = lappend(extra_options, defel);
		}
		else if (strcmp(defel->defname, "label") == 0)
		{
			if (labelEl != NULL)
				errorConflictingDefElem(defel, NULL);
			labelEl = defel;
			update_label = true;
			if (defel->arg != NULL)
			{
				char	   *str = defGetString(defel);

				if (pg_strcasecmp(str, "null") != 0 &&
					pg_strcasecmp(str, "none") != 0)
					label = cstring_to_text(str);
			}
		}
		else
		{
			extra_options = lappend(extra_options, defel);
		}
	}

	if (extra_options != NIL)
	{
		options_datum = deflist_to_options_array(extra_options);
		update_options = true;
	}

	EncodingContextUpdate(contextId,
						  enctab, update_enctab,
						  dectab, update_dectab,
						  label, update_label,
						  options_datum, update_options);

	ObjectAddressSet(address, EncodingContextRelationId, contextId);
	return address;
}
