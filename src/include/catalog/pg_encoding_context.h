/*-------------------------------------------------------------------------
 *
 * pg_encoding_context.h
 *	  definition of the "encoding context" system catalog (pg_encoding_context)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_encoding_context.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ENCODING_CONTEXT_H
#define PG_ENCODING_CONTEXT_H

#include "catalog/genbki.h"
#include "catalog/pg_encoding_context_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_encoding_context definition. cpp turns this into
 *		typedef struct FormData_pg_encoding_context
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_encoding_context,9431,EncodingContextRelationId)
{
	Oid			oid;			/* oid */

	/* context name */
	NameData	ectxname;

	/* namespace containing this context */
	Oid			ectxnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* owner of this context */
	Oid			ectxowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/* encoding type referenced by this context */
	Oid			ectxtypid BKI_LOOKUP(pg_encoding_type);

	/*
	 * optional parent encoding context for composed pipelines:
	 * encode runs parent first, then this context;
	 * decode runs this context first, then parent.
	 */
	Oid			ectxparentid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_encoding_context);

	/* target SQL data type (0 / InvalidOid for any; BYTEAOID when ectxparentid != 0) */
	Oid			ectxtargettypid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_type);

#ifdef CATALOG_VARLEN
	/* human-readable version / schema label */
	text		ectxlabel BKI_DEFAULT(_null_);

	/* raw encoding table / dictionary / schema / public key */
	bytea		ectxenctab BKI_DEFAULT(_null_);

	/* raw decoding table / dictionary / schema / private key (nullable) */
	bytea		ectxdectab BKI_DEFAULT(_null_);

	/* options (level, strategy, etc.) */
	text		ectxoptions[] BKI_DEFAULT(_null_);
#endif
} FormData_pg_encoding_context;

END_CATALOG_STRUCT

typedef FormData_pg_encoding_context *Form_pg_encoding_context;

DECLARE_UNIQUE_INDEX(pg_encoding_context_name_nsp_index, 9432, EncodingContextNameNspIndexId, pg_encoding_context, btree(ectxname name_ops, ectxnamespace oid_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_encoding_context_oid_index, 9433, EncodingContextObjectIdIndexId, pg_encoding_context, btree(oid oid_ops));

DECLARE_TOAST(pg_encoding_context, 9434, 9435);

MAKE_SYSCACHE(ENCODINGCONTEXTNAMENSP, pg_encoding_context_name_nsp_index, 8);
MAKE_SYSCACHE(ENCODINGCONTEXTOID, pg_encoding_context_oid_index, 8);

#endif							/* PG_ENCODING_CONTEXT_H */
