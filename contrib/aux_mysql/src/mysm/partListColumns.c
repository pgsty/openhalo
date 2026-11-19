/*-------------------------------------------------------------------------
 *
 * partListColumns.c
 *	  Tuple key formatter for MySQL PARTITION BY LIST COLUMNS emulation
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"
#include "halo_mysql.h"

#include "catalog/pg_type.h"
#include "mb/pg_wchar.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

static FmgrInfo *build_concat_foutcache(FunctionCallInfo fcinfo, int argidx);

PG_FUNCTION_INFO_V1(partListColumns);
Datum
partListColumns(PG_FUNCTION_ARGS)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;

	initStringInfo(&str);
	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, 0);

	appendStringInfoChar(&str, '(');
	for (int i = 0; i < PG_NARGS(); i++)
	{
		if (i > 0)
			appendStringInfoChar(&str, ',');

		if (!PG_ARGISNULL(i))
		{
			Datum		value = PG_GETARG_DATUM(i);
			Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
			Oid			valBaseType = mys_binary_base_type(valtype);

			if (valBaseType != BYTEAOID)
			{
				char	   *out = OutputFunctionCall(&foutcache[i], value);

				appendStringInfoString(&str, out);
				pfree(out);
			}
			else
			{
				bytea	   *vlena = DatumGetByteaPP(value);
				char	   *byte = VARDATA_ANY(vlena);
				size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);

				pg_verifymbstr(byte, (int) byteLen, false);
				appendBinaryStringInfo(&str, byte, byteLen);
				if ((Pointer) vlena != DatumGetPointer(value))
					pfree(vlena);
			}
		}
	}
	appendStringInfoChar(&str, ')');

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	PG_RETURN_TEXT_P(result);
}

static FmgrInfo *
build_concat_foutcache(FunctionCallInfo fcinfo, int argidx)
{
	FmgrInfo   *foutcache;
	int			nargs = PG_NARGS();
	int			i;

	foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
												Max(nargs, 1) * sizeof(FmgrInfo));

	for (i = argidx; i < nargs; i++)
	{
		Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
		Oid			typOutput;
		bool		typIsVarlena;

		if (!OidIsValid(valtype))
			ereport(ERROR,
					(errcode(ERRCODE_INDETERMINATE_DATATYPE),
					 errmsg("could not determine data type of partListColumns() input")));

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;
	return foutcache;
}
