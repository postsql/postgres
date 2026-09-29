/*-------------------------------------------------------------------------
 *
 * pg_encoding_type.h
 *	  definition of the "encoding type" system catalog (pg_encoding_type)
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_encoding_type.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_ENCODING_TYPE_H
#define PG_ENCODING_TYPE_H

#include "catalog/genbki.h"
#include "catalog/pg_encoding_type_d.h"	/* IWYU pragma: export */

/* ----------------
 *		pg_encoding_type definition. cpp turns this into
 *		typedef struct FormData_pg_encoding_type
 * ----------------
 */
BEGIN_CATALOG_STRUCT

CATALOG(pg_encoding_type,9441,EncodingTypeRelationId)
{
	Oid			oid;			/* oid */

	/* type name */
	NameData	enctypname;

	/* namespace containing this type */
	Oid			enctypnamespace BKI_DEFAULT(pg_catalog) BKI_LOOKUP(pg_namespace);

	/* owner of this type */
	Oid			enctypowner BKI_DEFAULT(POSTGRES) BKI_LOOKUP(pg_authid);

	/* high-level kind: 'c', 's', 'e', 'q', 't', 'd', 'm' */
	char		enctypkind;

	/* specific algorithm: "zstd", "avro", "huffman", etc. */
	NameData	enctyptype;

	/* custom encode function: enc_fn(internal, anyelement) -> bytea */
	regproc		enctypencfunc BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_proc);

	/* custom decode function: dec_fn(internal, bytea) -> anyelement */
	regproc		enctypdecfunc BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_proc);

	/* optional explain function: exp_fn(oid, text) -> json */
	regproc		enctypexpfunc BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_proc);
} FormData_pg_encoding_type;

END_CATALOG_STRUCT

typedef FormData_pg_encoding_type *Form_pg_encoding_type;

DECLARE_UNIQUE_INDEX(pg_encoding_type_name_nsp_index, 9442, EncodingTypeNameNspIndexId, pg_encoding_type, btree(enctypname name_ops, enctypnamespace oid_ops));
DECLARE_UNIQUE_INDEX_PKEY(pg_encoding_type_oid_index, 9443, EncodingTypeObjectIdIndexId, pg_encoding_type, btree(oid oid_ops));

MAKE_SYSCACHE(ENCODINGTYPENAMENSP, pg_encoding_type_name_nsp_index, 8);
MAKE_SYSCACHE(ENCODINGTYPEOID, pg_encoding_type_oid_index, 8);

#ifdef EXPOSE_TO_CLIENT_CODE

#define ENCTYPKIND_COMPRESSION		'c'
#define ENCTYPKIND_SERIALIZATION	's'
#define ENCTYPKIND_ENCRYPTION		'e'
#define ENCTYPKIND_QUANTIZATION		'q'
#define ENCTYPKIND_TOKENIZATION		't'
#define ENCTYPKIND_INTERNING		'd'
#define ENCTYPKIND_CUSTOM			'm'

#endif							/* EXPOSE_TO_CLIENT_CODE */

#endif							/* PG_ENCODING_TYPE_H */
