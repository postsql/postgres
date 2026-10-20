/*-------------------------------------------------------------------------
 *
 * pg_avro.c
 *	  Apache Avro 1.11.1 binary serialization, deserialization, schema
 *	  validation, and reader/writer schema evolution for PostgreSQL
 *	  encoding contexts.
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/pg_avro/pg_avro.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/encoding_context.h"
#include "access/htup_details.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/miscnodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/typcache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(pg_avro_validate_schema);
PG_FUNCTION_INFO_V1(pg_avro_context_encode);
PG_FUNCTION_INFO_V1(pg_avro_context_decode);

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

/*
 * Convert a JsonbValue (including jbvBinary) into a standalone Jsonb *
 * allocated in mcxt.
 */
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
		JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;

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

	/* Fallback to ectx->dectab / ectx->enctab if stored as UTF-8 JSON bytes */
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

static AvroSchemaNode *
get_avro_writer_schema(EncodingContext ectx)
{
	char	   *schema_str;
	AvroSchemaNode *node;

	if (ectx->enc_opaque != NULL)
		return (AvroSchemaNode *) ectx->enc_opaque;

	schema_str = avro_get_schema_string_from_ectx(ectx, false);
	node = avro_compile_schema_cstr(ectx->mcxt, schema_str);
	pfree(schema_str);

	ectx->enc_opaque = node;
	return node;
}

static AvroDecPair *
get_avro_dec_pair(EncodingContext ectx)
{
	AvroDecPair *pair;
	char	   *w_str;
	char	   *r_str;

	if (ectx->dec_opaque != NULL)
		return (AvroDecPair *) ectx->dec_opaque;

	w_str = avro_get_schema_string_from_ectx(ectx, false);
	r_str = avro_get_schema_string_from_ectx(ectx, true);

	pair = (AvroDecPair *) MemoryContextAllocZero(ectx->mcxt, sizeof(AvroDecPair));
	pair->writer_schema = avro_compile_schema_cstr(ectx->mcxt, w_str);
	if (strcmp(w_str, r_str) == 0)
		pair->reader_schema = pair->writer_schema;
	else
		pair->reader_schema = avro_compile_schema_cstr(ectx->mcxt, r_str);

	pfree(w_str);
	pfree(r_str);

	ectx->dec_opaque = pair;
	return pair;
}

/* ----------------------------------------------------------------
 *			Avro ZigZag + Varint & Binary Primitives
 * ----------------------------------------------------------------
 */

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

/* ----------------------------------------------------------------
 *			Avro JSONB -> Binary Encoder
 * ----------------------------------------------------------------
 */

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

	/*
	 * Check canonical Avro JSON union wrapper object: {"type_name": value}
	 */
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

	/*
	 * Natural JSON union matching with exact type/range/symbol/field checks.
	 */
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
		JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;

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
				/* Terminating 0-count block */
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
				/* Terminating 0-count block */
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

/* ----------------------------------------------------------------
 *			Avro SQL Datum / Composite -> Binary Encoder
 * ----------------------------------------------------------------
 */

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

/* ----------------------------------------------------------------
 *		Avro Binary -> JSONB Decoder (with Schema Resolution)
 * ----------------------------------------------------------------
 */

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

	/* Avro type promotion rules */
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

	/* Resolve writer union first */
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

	/* Resolve reader union */
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
		JsonbIteratorToken tok PG_USED_FOR_ASSERTS_ONLY;

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

/* ----------------------------------------------------------------
 *		Avro Binary -> SQL Datum / Composite Decoder
 * ----------------------------------------------------------------
 */

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

/* ----------------------------------------------------------------
 *					SQL-Callable Extension Entrypoints
 * ----------------------------------------------------------------
 */

/*
 * pg_avro_validate_schema(schema_json text) RETURNS bool
 */
Datum
pg_avro_validate_schema(PG_FUNCTION_ARGS)
{
	text	   *schema_txt = PG_GETARG_TEXT_PP(0);
	char	   *schema_cstr = text_to_cstring(schema_txt);
	MemoryContext tmpcxt;

	tmpcxt = AllocSetContextCreate(CurrentMemoryContext,
								   "AvroSchemaValidateTmp",
								   ALLOCSET_DEFAULT_SIZES);
	(void) avro_compile_schema_cstr(tmpcxt, schema_cstr);
	MemoryContextDelete(tmpcxt);
	pfree(schema_cstr);

	PG_RETURN_BOOL(true);
}

/*
 * pg_avro_context_encode(internal, anyelement) RETURNS bytea
 */
Datum
pg_avro_context_encode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	Datum		val = PG_GETARG_DATUM(1);
	Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, 1);
	AvroSchemaNode *w_schema;
	StringInfoData buf;
	bytea	   *result;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_avro_context_encode called with NULL EncodingContext")));

	if (!OidIsValid(valtype))
		valtype = OidIsValid(ectx->target_typid) ? ectx->target_typid : JSONBOID;

	w_schema = get_avro_writer_schema(ectx);
	initStringInfo(&buf);

	avro_encode_sql_datum(&buf, w_schema, val, false, valtype);

	result = (bytea *) palloc(VARHDRSZ + buf.len);
	SET_VARSIZE(result, VARHDRSZ + buf.len);
	if (buf.len > 0)
		memcpy(VARDATA(result), buf.data, buf.len);
	pfree(buf.data);

	PG_RETURN_BYTEA_P(result);
}

/*
 * pg_avro_context_decode(internal, bytea) RETURNS anyelement
 */
Datum
pg_avro_context_decode(PG_FUNCTION_ARGS)
{
	EncodingContext ectx = (EncodingContext) PG_GETARG_POINTER(0);
	bytea	   *encoded = PG_GETARG_BYTEA_PP(1);
	Oid			resulttype = get_fn_expr_rettype(fcinfo->flinfo);
	AvroDecPair *pair;
	const uint8 *ptr;
	const uint8 *end;
	bool		isnull = false;
	Datum		res;

	if (ectx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("pg_avro_context_decode called with NULL EncodingContext")));

	pair = get_avro_dec_pair(ectx);

	if (!OidIsValid(resulttype) ||
		(resulttype == BYTEAOID && !OidIsValid(ectx->target_typid) &&
		 pair->reader_schema->kind != AVRO_BYTES &&
		 pair->reader_schema->kind != AVRO_FIXED))
	{
		resulttype = OidIsValid(ectx->target_typid) ? ectx->target_typid : JSONBOID;
	}

	ptr = (const uint8 *) VARDATA_ANY(encoded);
	end = ptr + VARSIZE_ANY_EXHDR(encoded);

	res = avro_decode_to_sql_datum(pair->writer_schema, pair->reader_schema,
								   &ptr, end, resulttype, &isnull);
	if (ptr != end)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("Avro payload has %td trailing unconsumed bytes", end - ptr)));

	if (isnull)
		PG_RETURN_NULL();
	return res;
}
