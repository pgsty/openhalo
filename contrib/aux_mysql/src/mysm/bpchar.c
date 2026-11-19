/*-------------------------------------------------------------------------
 *
 * bpchar.c
 *	  MySQL-compatible bpchar and text comparison/LIKE routines
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "catalog/pg_collation.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/pg_locale.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(char_eq_char_for_date_format);
Datum
char_eq_char_for_date_format(PG_FUNCTION_ARGS)
{
	BpChar	   *arg1;
	BpChar	   *arg2;
	int			len1;
	int			len2;
	const char *str1;
	const char *str2;
	bool		result;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_BOOL(false);

	arg1 = PG_GETARG_BPCHAR_PP(0);
	arg2 = PG_GETARG_BPCHAR_PP(1);
	len1 = VARSIZE_ANY_EXHDR(arg1);
	len2 = VARSIZE_ANY_EXHDR(arg2);
	str1 = VARDATA_ANY(arg1);
	str2 = VARDATA_ANY(arg2);

	result = (len1 == len2 && len1 > 0 && memcmp(str1, str2, len1) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	PG_RETURN_BOOL(result);
}

/*
 * Compare two text values for equality using MySQL PAD SPACE semantics
 * (trailing ASCII spaces stripped before collation-aware comparison).
 */
static bool
mys_text_eq_internal(FunctionCallInfo fcinfo)
{
	Oid			collid = PG_GET_COLLATION();
	pg_locale_t mylocale;
	text	   *arg1 = PG_GETARG_TEXT_PP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	const char *str1 = VARDATA_ANY(arg1);
	const char *str2 = VARDATA_ANY(arg2);
	int			len1 = VARSIZE_ANY_EXHDR(arg1);
	int			len2 = VARSIZE_ANY_EXHDR(arg2);
	bool		result;

	if (!OidIsValid(collid))
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
	mylocale = pg_newlocale_from_collation(collid);

	while (len1 > 0 && str1[len1 - 1] == ' ')
		len1--;
	while (len2 > 0 && str2[len2 - 1] == ' ')
		len2--;

	if (mylocale->deterministic)
		result = (len1 == len2 && memcmp(str1, str2, len1) == 0);
	else
		result = (varstr_cmp(str1, len1, str2, len2, collid) == 0);

	PG_FREE_IF_COPY(arg1, 0);
	PG_FREE_IF_COPY(arg2, 1);

	return result;
}

PG_FUNCTION_INFO_V1(texteq_mys);
Datum
texteq_mys(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(mys_text_eq_internal(fcinfo));
}

PG_FUNCTION_INFO_V1(textne_mys);
Datum
textne_mys(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(!mys_text_eq_internal(fcinfo));
}

PG_FUNCTION_INFO_V1(bpcharlike);
Datum
bpcharlike(PG_FUNCTION_ARGS)
{
	BpChar	   *str = PG_GETARG_BPCHAR_PP(0);
	Datum		pat = PG_GETARG_DATUM(1);
	const char *s = VARDATA_ANY(str);
	int			slen = VARSIZE_ANY_EXHDR(str);
	text	   *trimmed;
	Datum		res;

	while (slen > 0 && s[slen - 1] == ' ')
		slen--;

	trimmed = cstring_to_text_with_len(s, slen);
	res = DirectFunctionCall2Coll(textlike,
								  PG_GET_COLLATION(),
								  PointerGetDatum(trimmed),
								  pat);

	pfree(trimmed);
	PG_FREE_IF_COPY(str, 0);

	return res;
}

PG_FUNCTION_INFO_V1(bpcharnlike);
Datum
bpcharnlike(PG_FUNCTION_ARGS)
{
	BpChar	   *str = PG_GETARG_BPCHAR_PP(0);
	Datum		pat = PG_GETARG_DATUM(1);
	const char *s = VARDATA_ANY(str);
	int			slen = VARSIZE_ANY_EXHDR(str);
	text	   *trimmed;
	Datum		res;

	while (slen > 0 && s[slen - 1] == ' ')
		slen--;

	trimmed = cstring_to_text_with_len(s, slen);
	res = DirectFunctionCall2Coll(textnlike,
								  PG_GET_COLLATION(),
								  PointerGetDatum(trimmed),
								  pat);

	pfree(trimmed);
	PG_FREE_IF_COPY(str, 0);

	return res;
}
