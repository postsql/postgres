/*-------------------------------------------------------------------------
 *
 * invisible_indexes.c
 *    PostgreSQL extension to support invisible / excluded indexes.
 *
 * Copyright (c) 2026, Google LLC.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "fmgr.h"

#include <ctype.h>

#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/relation.h"
#endif
#include "access/reloptions.h"
#include "catalog/namespace.h"
#if PG_VERSION_NUM >= 110000
#include "catalog/partition.h"
#endif
#include "catalog/pg_class.h"
#include "catalog/pg_type.h"
#include "commands/comment.h"
#include "commands/explain.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#if PG_VERSION_NUM >= 120000
#include "nodes/pathnodes.h"
#else
#include "nodes/relation.h"
#endif
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#if PG_VERSION_NUM >= 100000
#include "utils/varlena.h"
#endif

#include "invisible_indexes.h"

PG_MODULE_MAGIC;

/* GUC variables */
bool invisible_indexes_enabled = true;
bool invisible_indexes_use_invisible = false;
char *excluded_indexes_string = NULL;
char *invisible_indexes_excluded_string = NULL;

#if PG_VERSION_NUM >= 190000
static build_simple_rel_hook_type prev_build_simple_rel_hook = NULL;
#else
static get_relation_info_hook_type prev_get_relation_info_hook = NULL;
#endif

typedef struct ExcludedIndexPattern
{
	char *schemaname; /* NULL if unqualified */
	char *indexname;  /* index name */
} ExcludedIndexPattern;

static MemoryContext InvisibleIndexesContext = NULL;
static char *cached_guc_excluded = NULL;
static char *cached_guc_inv_excluded = NULL;
static List *cached_patterns = NIL;

PG_FUNCTION_INFO_V1(invisible_indexes_set_invisible);
PG_FUNCTION_INFO_V1(invisible_indexes_set_invisible_oid);
PG_FUNCTION_INFO_V1(invisible_indexes_is_invisible);
PG_FUNCTION_INFO_V1(invisible_indexes_is_invisible_oid);
PG_FUNCTION_INFO_V1(invisible_indexes_version);

static bool is_index_invisible(Oid indexoid);
static bool is_single_index_invisible(Oid indexoid);
static bool is_index_in_patterns(Oid indexoid);
static bool is_index_comment_invisible(Oid indexoid);
static bool is_index_reloption_invisible(Oid indexoid);
static void filter_invisible_indexes(PlannerInfo *root, Oid relationObjectId, RelOptInfo *rel);
static void update_cached_patterns_if_needed(void);
static void parse_and_append_patterns(const char *str, List **patterns);
static bool invisible_indexes_set_invisible_internal(Oid indexoid, bool make_invisible);

#if PG_VERSION_NUM >= 190000
static void
invisible_indexes_build_simple_rel_hook(PlannerInfo *root,
                                        RelOptInfo *rel,
                                        RangeTblEntry *rte)
{
	if (prev_build_simple_rel_hook)
		prev_build_simple_rel_hook(root, rel, rte);

	if (rte->rtekind == RTE_RELATION && OidIsValid(rte->relid))
	{
		filter_invisible_indexes(root, rte->relid, rel);
	}
}
#else
static void
invisible_indexes_get_relation_info_hook(PlannerInfo *root,
                                         Oid relationObjectId,
                                         bool inhparent,
                                         RelOptInfo *rel)
{
	if (prev_get_relation_info_hook)
		prev_get_relation_info_hook(root, relationObjectId, inhparent, rel);

	filter_invisible_indexes(root, relationObjectId, rel);
}
#endif

/*
 * Parse a comma-separated list of index identifiers (supporting quotes and schema qualifiers)
 * and append pattern items to *patterns.
 */
static void
parse_and_append_patterns(const char *str, List **patterns)
{
	const char *p;

	if (str == NULL || str[0] == '\0')
		return;

	p = str;
	while (*p)
	{
		const char *item_start;
		int item_len;
		bool in_quotes = false;

		CHECK_FOR_INTERRUPTS();

		while (*p && isspace((unsigned char) *p))
			p++;

		if (*p == '\0')
			break;

		if (*p == ',')
		{
			p++;
			continue;
		}

		item_start = p;
		while (*p && (in_quotes || *p != ','))
		{
			if (*p == '"')
			{
				if (in_quotes && *(p + 1) == '"')
					p += 2;
				else
				{
					in_quotes = !in_quotes;
					p++;
				}
			}
			else
			{
				p++;
			}
		}

		item_len = p - item_start;
		while (item_len > 0 && isspace((unsigned char) item_start[item_len - 1]))
			item_len--;

		if (item_len > 0)
		{
			char *raw_item = pnstrdup(item_start, item_len);
			List *parts = NIL;

			if (SplitIdentifierString(raw_item, '.', &parts) && parts != NIL)
			{
				ExcludedIndexPattern *pat = (ExcludedIndexPattern *) palloc0(sizeof(ExcludedIndexPattern));

				if (list_length(parts) == 1)
				{
					pat->schemaname = NULL;
					pat->indexname = pstrdup((char *) linitial(parts));
					*patterns = lappend(*patterns, pat);
				}
				else if (list_length(parts) == 2)
				{
					pat->schemaname = pstrdup((char *) linitial(parts));
					pat->indexname = pstrdup((char *) lsecond(parts));
					*patterns = lappend(*patterns, pat);
				}
				else if (list_length(parts) >= 3)
				{
					pat->schemaname = pstrdup((char *) list_nth(parts, list_length(parts) - 2));
					pat->indexname = pstrdup((char *) list_nth(parts, list_length(parts) - 1));
					*patterns = lappend(*patterns, pat);
				}
			}
			pfree(raw_item);
			list_free(parts);
		}

		if (*p == ',')
			p++;
	}
}

/*
 * Refresh cached GUC patterns if either GUC string changed.
 */
static void
update_cached_patterns_if_needed(void)
{
	const char *s1 = excluded_indexes_string;
	const char *s2 = invisible_indexes_excluded_string;
	bool changed = false;

	if (s1 == NULL)
		s1 = "";
	if (s2 == NULL)
		s2 = "";

	if (cached_guc_excluded == NULL || strcmp(s1, cached_guc_excluded) != 0)
		changed = true;
	if (cached_guc_inv_excluded == NULL || strcmp(s2, cached_guc_inv_excluded) != 0)
		changed = true;

	if (!changed)
		return;

	if (InvisibleIndexesContext == NULL)
	{
		InvisibleIndexesContext = AllocSetContextCreate(TopMemoryContext,
		                                                "InvisibleIndexesContext",
		                                                ALLOCSET_DEFAULT_SIZES);
	}
	else
	{
		MemoryContextReset(InvisibleIndexesContext);
	}

	{
		MemoryContext oldcontext = MemoryContextSwitchTo(InvisibleIndexesContext);

		cached_guc_excluded = pstrdup(s1);
		cached_guc_inv_excluded = pstrdup(s2);
		cached_patterns = NIL;

		parse_and_append_patterns(s1, &cached_patterns);
		parse_and_append_patterns(s2, &cached_patterns);

		MemoryContextSwitchTo(oldcontext);
	}
}

/*
 * Match indexoid against parsed GUC patterns.
 */
static bool
is_index_in_patterns(Oid indexoid)
{
	ListCell *lc;
	char *idxname;
	char *nspname = NULL;

	if (cached_patterns == NIL)
		return false;

	idxname = get_rel_name(indexoid);
	if (idxname == NULL)
		return false;

	foreach(lc, cached_patterns)
	{
		ExcludedIndexPattern *pat = (ExcludedIndexPattern *) lfirst(lc);

		CHECK_FOR_INTERRUPTS();

		if (strcmp(idxname, pat->indexname) == 0)
		{
			if (pat->schemaname == NULL)
				return true;

			if (nspname == NULL)
				nspname = get_namespace_name(get_rel_namespace(indexoid));

			if (nspname != NULL && strcmp(nspname, pat->schemaname) == 0)
				return true;
		}
	}

	return false;
}

/*
 * Check if the index has the comment 'invisible' or contains '@invisible'.
 */
static bool
is_index_comment_invisible(Oid indexoid)
{
	char *comment;
	bool is_invisible = false;

	comment = GetComment(indexoid, RelationRelationId, 0);
	if (comment != NULL)
	{
		if (pg_strcasecmp(comment, "invisible") == 0 ||
		    strstr(comment, "@invisible") != NULL)
			is_invisible = true;
		pfree(comment);
	}
	return is_invisible;
}

/*
 * Check if the index has the reloption 'invisible = true'.
 */
static bool
is_index_reloption_invisible(Oid indexoid)
{
	HeapTuple tuple;
	bool is_invisible = false;

	tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(indexoid));
	if (HeapTupleIsValid(tuple))
	{
		bool isnull;
		Datum datum = SysCacheGetAttr(RELOID, tuple, Anum_pg_class_reloptions, &isnull);

		if (!isnull)
		{
			ArrayType *arr = DatumGetArrayTypePCopy(datum);
			int nelems;
			Datum *elems;
			bool *nulls;
			int i;

			deconstruct_array(arr, TEXTOID, -1, false, TYPALIGN_INT,
			                  &elems, &nulls, &nelems);
			for (i = 0; i < nelems; i++)
			{
				CHECK_FOR_INTERRUPTS();

				if (!nulls[i])
				{
					char *str = TextDatumGetCString(elems[i]);

					if (pg_strcasecmp(str, "invisible=true") == 0 ||
					    pg_strcasecmp(str, "invisible=on") == 0 ||
					    pg_strcasecmp(str, "invisible=1") == 0 ||
					    pg_strcasecmp(str, "invisible=yes") == 0)
					{
						is_invisible = true;
						pfree(str);
						break;
					}
					pfree(str);
				}
			}
			pfree(elems);
			pfree(nulls);
			pfree(arr);
		}
		ReleaseSysCache(tuple);
	}

	return is_invisible;
}

/*
 * Check a single relation index for invisibility.
 */
static bool
is_single_index_invisible(Oid indexoid)
{
	if (is_index_in_patterns(indexoid))
		return true;

	if (is_index_comment_invisible(indexoid))
		return true;

	if (is_index_reloption_invisible(indexoid))
		return true;

	return false;
}

/*
 * Check if a given index OID is invisible or excluded, walking partition parent hierarchy.
 */
static bool
is_index_invisible(Oid indexoid)
{
	Oid curroid;

	if (!invisible_indexes_enabled || invisible_indexes_use_invisible)
		return false;

	update_cached_patterns_if_needed();

	curroid = indexoid;
	while (OidIsValid(curroid))
	{
		CHECK_FOR_INTERRUPTS();

		if (is_single_index_invisible(curroid))
			return true;

#if PG_VERSION_NUM >= 110000
		/* If curroid is a partition of a parent index, check parent index */
		if (get_rel_relispartition(curroid))
		{
#if PG_VERSION_NUM >= 140000
			curroid = get_partition_parent(curroid, false);
#else
			curroid = get_partition_parent(curroid);
#endif
		}
		else
		{
			break;
		}
#else
		break;
#endif
	}

	return false;
}

/*
 * Filter out invisible / excluded indexes from rel->indexlist.
 */
static void
filter_invisible_indexes(PlannerInfo *root, Oid relationObjectId, RelOptInfo *rel)
{
	List *filtered_indexes;
	ListCell *lc;

	/* If disabled globally or optimizer is told to use invisible indexes, skip */
	if (!invisible_indexes_enabled || invisible_indexes_use_invisible)
		return;

	if (rel == NULL || rel->indexlist == NIL)
		return;

	update_cached_patterns_if_needed();

	filtered_indexes = NIL;
	foreach(lc, rel->indexlist)
	{
		IndexOptInfo *info = (IndexOptInfo *) lfirst(lc);

		CHECK_FOR_INTERRUPTS();

		if (is_index_invisible(info->indexoid))
		{
			/* Excluded from planning */
			continue;
		}

		filtered_indexes = lappend(filtered_indexes, info);
	}

	rel->indexlist = filtered_indexes;
}

/*
 * Extension initialization.
 */
void
_PG_init(void)
{
	/* Register custom GUCs */
	DefineCustomStringVariable("excluded_indexes",
	                           "Comma-separated list of indexes to exclude from query planning",
	                           NULL,
	                           &excluded_indexes_string,
	                           "",
	                           PGC_USERSET,
	                           0,
	                           NULL, NULL, NULL);

	DefineCustomStringVariable("invisible_indexes.excluded_indexes",
	                           "Comma-separated list of indexes to exclude from query planning",
	                           NULL,
	                           &invisible_indexes_excluded_string,
	                           "",
	                           PGC_USERSET,
	                           0,
	                           NULL, NULL, NULL);

	DefineCustomBoolVariable("invisible_indexes.use_invisible_indexes",
	                         "Allow query optimizer to use invisible/excluded indexes (Oracle/MySQL compatibility)",
	                         NULL,
	                         &invisible_indexes_use_invisible,
	                         false,
	                         PGC_USERSET,
	                         0,
	                         NULL, NULL, NULL);

	DefineCustomBoolVariable("invisible_indexes.enabled",
	                         "Enable or disable the invisible_indexes optimizer hook",
	                         NULL,
	                         &invisible_indexes_enabled,
	                         true,
	                         PGC_USERSET,
	                         0,
	                         NULL, NULL, NULL);

	EmitWarningsOnPlaceholders("invisible_indexes");

	/* Install planner hook */
#if PG_VERSION_NUM >= 190000
	prev_build_simple_rel_hook = build_simple_rel_hook;
	build_simple_rel_hook = invisible_indexes_build_simple_rel_hook;
#else
	prev_get_relation_info_hook = get_relation_info_hook;
	get_relation_info_hook = invisible_indexes_get_relation_info_hook;
#endif
}

/*
 * Extension cleanup.
 */
void
_PG_fini(void)
{
#if PG_VERSION_NUM >= 190000
	build_simple_rel_hook = prev_build_simple_rel_hook;
#else
	get_relation_info_hook = prev_get_relation_info_hook;
#endif

	if (InvisibleIndexesContext != NULL)
	{
		MemoryContextDelete(InvisibleIndexesContext);
		InvisibleIndexesContext = NULL;
	}
	cached_guc_excluded = NULL;
	cached_guc_inv_excluded = NULL;
	cached_patterns = NIL;
}

/*
 * Helper to update comment metadata for index visibility.
 */
static bool
invisible_indexes_set_invisible_internal(Oid indexoid, bool make_invisible)
{
	Relation rel;
	char *comment;

	rel = relation_open(indexoid, ShareUpdateExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_INDEX
#ifdef RELKIND_PARTITIONED_INDEX
	    && rel->rd_rel->relkind != RELKIND_PARTITIONED_INDEX
#endif
	   )
	{
		relation_close(rel, ShareUpdateExclusiveLock);
		ereport(ERROR,
		        (errcode(ERRCODE_WRONG_OBJECT_TYPE),
		         errmsg("\"%s\" is not an index", RelationGetRelationName(rel))));
	}
	relation_close(rel, ShareUpdateExclusiveLock);

	comment = GetComment(indexoid, RelationRelationId, 0);

	if (make_invisible)
	{
		if (comment == NULL)
		{
			CreateComments(indexoid, RelationRelationId, 0, "invisible");
		}
		else if (pg_strcasecmp(comment, "invisible") != 0 &&
		         strstr(comment, "@invisible") == NULL)
		{
			char *new_comment = psprintf("%s @invisible", comment);
			CreateComments(indexoid, RelationRelationId, 0, new_comment);
			pfree(new_comment);
		}
	}
	else
	{
		if (comment != NULL)
		{
			if (pg_strcasecmp(comment, "invisible") == 0)
			{
				DeleteComments(indexoid, RelationRelationId, 0);
			}
			else if (strstr(comment, "@invisible") != NULL)
			{
				StringInfoData buf;
				char *pos = strstr(comment, "@invisible");
				initStringInfo(&buf);
				appendBinaryStringInfo(&buf, comment, pos - comment);
				appendStringInfoString(&buf, pos + strlen("@invisible"));
				while (buf.len > 0 && isspace((unsigned char) buf.data[buf.len - 1]))
				{
					buf.data[--buf.len] = '\0';
				}
				if (buf.len == 0)
					DeleteComments(indexoid, RelationRelationId, 0);
				else
					CreateComments(indexoid, RelationRelationId, 0, buf.data);
				pfree(buf.data);
			}
		}
	}

	if (comment != NULL)
		pfree(comment);

	return true;
}

/*
 * SQL Function: invisible_indexes.set_invisible(text, boolean)
 */
Datum
invisible_indexes_set_invisible(PG_FUNCTION_ARGS)
{
	text *relname_text;
	bool make_invisible;
	RangeVar *relvar;
	Oid indexoid;

	/* Superuser check as required by PostgreSQL development guidelines */
	if (!superuser())
		ereport(ERROR,
		        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		         errmsg("must be superuser to modify index visibility")));

	if (PG_ARGISNULL(0))
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("index name cannot be null")));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("visibility flag cannot be null")));

	relname_text = PG_GETARG_TEXT_PP(0);
	make_invisible = PG_GETARG_BOOL(1);

	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	indexoid = RangeVarGetRelid(relvar, ShareUpdateExclusiveLock, false);

	invisible_indexes_set_invisible_internal(indexoid, make_invisible);

	PG_RETURN_BOOL(true);
}

/*
 * SQL Function: invisible_indexes.set_invisible(regclass, boolean)
 */
Datum
invisible_indexes_set_invisible_oid(PG_FUNCTION_ARGS)
{
	Oid indexoid;
	bool make_invisible;

	/* Superuser check as required by PostgreSQL development guidelines */
	if (!superuser())
		ereport(ERROR,
		        (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		         errmsg("must be superuser to modify index visibility")));

	if (PG_ARGISNULL(0))
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("index OID cannot be null")));

	if (PG_ARGISNULL(1))
		ereport(ERROR,
		        (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
		         errmsg("visibility flag cannot be null")));

	indexoid = PG_GETARG_OID(0);
	make_invisible = PG_GETARG_BOOL(1);

	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid)))
		ereport(ERROR,
		        (errcode(ERRCODE_UNDEFINED_OBJECT),
		         errmsg("relation with OID %u does not exist", indexoid)));

	invisible_indexes_set_invisible_internal(indexoid, make_invisible);

	PG_RETURN_BOOL(true);
}

/*
 * SQL Function: invisible_indexes.is_invisible(text)
 */
Datum
invisible_indexes_is_invisible(PG_FUNCTION_ARGS)
{
	text *relname_text;
	RangeVar *relvar;
	Oid indexoid;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	relname_text = PG_GETARG_TEXT_PP(0);
	relvar = makeRangeVarFromNameList(textToQualifiedNameList(relname_text));
	indexoid = RangeVarGetRelid(relvar, NoLock, true);
	if (!OidIsValid(indexoid))
		PG_RETURN_NULL();

	{
		char relkind = get_rel_relkind(indexoid);
		if (relkind != RELKIND_INDEX
#ifdef RELKIND_PARTITIONED_INDEX
		    && relkind != RELKIND_PARTITIONED_INDEX
#endif
		   )
			PG_RETURN_NULL();
	}

	PG_RETURN_BOOL(is_index_invisible(indexoid));
}

/*
 * SQL Function: invisible_indexes.is_invisible(regclass)
 */
Datum
invisible_indexes_is_invisible_oid(PG_FUNCTION_ARGS)
{
	Oid indexoid;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	indexoid = PG_GETARG_OID(0);
	if (!SearchSysCacheExists1(RELOID, ObjectIdGetDatum(indexoid)))
		PG_RETURN_NULL();

	{
		char relkind = get_rel_relkind(indexoid);
		if (relkind != RELKIND_INDEX
#ifdef RELKIND_PARTITIONED_INDEX
		    && relkind != RELKIND_PARTITIONED_INDEX
#endif
		   )
			PG_RETURN_NULL();
	}

	PG_RETURN_BOOL(is_index_invisible(indexoid));
}

/*
 * SQL Function: invisible_indexes.version()
 */
Datum
invisible_indexes_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text("1.0"));
}
