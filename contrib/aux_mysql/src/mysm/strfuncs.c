/*-------------------------------------------------------------------------
 *
 * strfuncs.c
 *	  MySQL-compatible string, numeric conversion, and bit helper functions
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

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <strings.h>

#include "access/detoast.h"
#include "adapter/mysql/adapter.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "common/int.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/varbit.h"
#include "utils/varlena.h"

typedef struct
{
	bool		is_multibyte;
	bool		is_multibyte_char_in_char;

	char	   *str1;			/* haystack string */
	char	   *str2;			/* needle string */
	int			len1;			/* string lengths in bytes */
	int			len2;

	int			skiptablemask;
	int			skiptable[256];

	char	   *last_match;
	char	   *refpoint;
	int			refpos;
} TextPositionState;

static text *text_substring(Datum str, int32 start, int32 length, bool length_not_specified);
static text *concat_internal(const char *sepstr, int argidx, FunctionCallInfo fcinfo, bool skip_nulls);
static FmgrInfo *build_concat_foutcache(FunctionCallInfo fcinfo, int argidx);
static int	text_position(text *t1, text *t2, Oid collid);
static void text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state);
static bool text_position_next(Oid collid, TextPositionState *state);
static char *text_position_next_internal(Oid collid, char *start_ptr, TextPositionState *state);
static int	text_position_get_match_pos(TextPositionState *state);
static void check_collation_set_local(Oid collid);
static void convert_to_digit_text_for_mysql(char **str, int str_len);
static void convert_to_text_for_mysql(char **str, int str_len);
static text *ascii_lower_copy(const text *src);
static VarBit *mys_int64_to_varbit(int64 val);

PG_FUNCTION_INFO_V1(elt);
Datum
elt(PG_FUNCTION_ARGS)
{
	FmgrInfo   *foutcache;
	Datum		datum;
	char	   *value;
	char	   *endptr;
	double		dindex;
	int64		index;
	Oid			valtype;
	Oid			valBaseType;
	text	   *res;

	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, 0);

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	datum = PG_GETARG_DATUM(0);
	value = OutputFunctionCall(&foutcache[0], datum);
	errno = 0;
	dindex = strtod(value, &endptr);
	pfree(value);

	if (isnan(dindex) || isinf(dindex) || dindex < 1.0 || dindex >= (double) PG_NARGS() + 0.5)
		PG_RETURN_NULL();

	index = (int64) llrint(dindex);
	if (index < 1 || index >= PG_NARGS())
		PG_RETURN_NULL();

	if (PG_ARGISNULL((int) index))
		PG_RETURN_NULL();

	datum = PG_GETARG_DATUM((int) index);
	valtype = get_fn_expr_argtype(fcinfo->flinfo, (int) index);
	valBaseType = mys_binary_base_type(valtype);
	if (valBaseType != BYTEAOID)
	{
		value = OutputFunctionCall(&foutcache[(int) index], datum);
		res = cstring_to_text(value);
		pfree(value);
	}
	else
	{
		bytea	   *vlena = DatumGetByteaPP(datum);
		const char *bytes = VARDATA_ANY(vlena);
		int			blen = VARSIZE_ANY_EXHDR(vlena);

		pg_verifymbstr(bytes, blen, false);
		res = cstring_to_text_with_len(bytes, blen);
		if ((Pointer) vlena != DatumGetPointer(datum))
			pfree(vlena);
	}

	PG_RETURN_TEXT_P(res);
}

PG_FUNCTION_INFO_V1(text_substr);
Datum
text_substr(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									PG_GETARG_INT32(2),
									false));
}

PG_FUNCTION_INFO_V1(text_substr_no_len);
Datum
text_substr_no_len(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									PG_GETARG_INT32(1),
									-1,
									true));
}

PG_FUNCTION_INFO_V1(text_left);
Datum
text_left(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(text_substring(PG_GETARG_DATUM(0),
									1,
									PG_GETARG_INT32(1),
									false));
}

PG_FUNCTION_INFO_V1(text_right);
Datum
text_right(PG_FUNCTION_ARGS)
{
	Datum		arg1;
	int			len1;
	int			rightLen;
	int			startIndex;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	arg1 = PG_GETARG_DATUM(0);
	len1 = DatumGetInt32(DirectFunctionCall1(textlen, arg1));
	rightLen = PG_GETARG_INT32(1);

	if (rightLen <= 0)
		PG_RETURN_TEXT_P(cstring_to_text(""));

	startIndex = (rightLen < len1) ? (len1 - rightLen + 1) : 1;
	PG_RETURN_TEXT_P(text_substring(arg1, startIndex, -1, true));
}

PG_FUNCTION_INFO_V1(check_set);
Datum
check_set(PG_FUNCTION_ARGS)
{
	text	   *txt1;
	text	   *txt2;
	const char *str1;
	const char *str2;
	int			str1Len;
	int			str2Len;
	const char *item_ptrs[64];
	int			item_lens[64];
	int			items1Num = 0;
	int			pos;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_BOOL(false);

	txt1 = PG_GETARG_TEXT_PP(0);
	txt2 = PG_GETARG_TEXT_PP(1);
	str1 = VARDATA_ANY(txt1);
	str1Len = VARSIZE_ANY_EXHDR(txt1);
	str2 = VARDATA_ANY(txt2);
	str2Len = VARSIZE_ANY_EXHDR(txt2);

	if (str2Len == 0)
	{
		PG_FREE_IF_COPY(txt1, 0);
		PG_FREE_IF_COPY(txt2, 1);
		PG_RETURN_BOOL(true);
	}

	pos = 0;
	while (pos <= str1Len)
	{
		int			end = pos;
		int			tlen;

		while (end < str1Len && str1[end] != ',')
			end++;

		tlen = end - pos;
		while (tlen > 0 && str1[pos + tlen - 1] == ' ')
			tlen--;

		if (tlen > 0)
		{
			if (items1Num >= 64)
			{
				PG_FREE_IF_COPY(txt1, 0);
				PG_FREE_IF_COPY(txt2, 1);
				PG_RETURN_BOOL(false);
			}
			item_ptrs[items1Num] = str1 + pos;
			item_lens[items1Num] = tlen;
			items1Num++;
		}

		if (end == str1Len)
			break;
		pos = end + 1;
	}

	pos = 0;
	while (pos <= str2Len)
	{
		int			end = pos;
		int			tlen;
		bool		found = false;

		while (end < str2Len && str2[end] != ',')
			end++;

		tlen = end - pos;
		if (end == str2Len)
		{
			while (tlen > 0 && str2[pos + tlen - 1] == ' ')
				tlen--;
		}

		for (int l = 0; l < items1Num; l++)
		{
			if (item_lens[l] == tlen &&
				pg_strncasecmp(str2 + pos, item_ptrs[l], tlen) == 0)
			{
				found = true;
				break;
			}
		}

		if (!found)
		{
			PG_FREE_IF_COPY(txt1, 0);
			PG_FREE_IF_COPY(txt2, 1);
			PG_RETURN_BOOL(false);
		}

		if (end == str2Len)
			break;
		pos = end + 1;
	}

	PG_FREE_IF_COPY(txt1, 0);
	PG_FREE_IF_COPY(txt2, 1);
	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(text_instr);
Datum
text_instr(PG_FUNCTION_ARGS)
{
	text	   *str;
	text	   *search_str;
	int32		res;

	if (mys_is_mysql_session())
		return mys_string_instr(fcinfo);

	str = PG_GETARG_TEXT_PP(0);
	search_str = PG_GETARG_TEXT_PP(1);
	res = (int32) text_position(str, search_str, PG_GET_COLLATION());
	PG_FREE_IF_COPY(str, 0);
	PG_FREE_IF_COPY(search_str, 1);
	PG_RETURN_INT32(res);
}

PG_FUNCTION_INFO_V1(convert_time_text_to_numeric_for_mysql);
Datum
convert_time_text_to_numeric_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *str;
	int			strLen;
	char		tmp[64];
	int			j = 0;
	float8		ret;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	str = TextDatumGetCString(PG_GETARG_DATUM(0));
	strLen = strlen(str);
	if (strLen == 0 || strLen >= (int) sizeof(tmp))
	{
		pfree(str);
		PG_RETURN_NULL();
	}

	for (int i = 0; i < strLen; i++)
	{
		if (isdigit((unsigned char) str[i]) || str[i] == '.')
			tmp[j++] = str[i];
	}
	tmp[j] = '\0';
	pfree(str);

	ret = strtod(tmp, NULL);
	PG_RETURN_FLOAT8(ret);
}

PG_FUNCTION_INFO_V1(text_locate2);
Datum
text_locate2(PG_FUNCTION_ARGS)
{
	text	   *str;
	text	   *search_str;
	int32		res;

	if (mys_is_mysql_session())
		return mys_string_locate(fcinfo);

	str = PG_GETARG_TEXT_PP(0);
	search_str = PG_GETARG_TEXT_PP(1);
	res = (int32) text_position(search_str, str, PG_GET_COLLATION());
	PG_FREE_IF_COPY(str, 0);
	PG_FREE_IF_COPY(search_str, 1);
	PG_RETURN_INT32(res);
}

PG_FUNCTION_INFO_V1(text_locate3);
Datum
text_locate3(PG_FUNCTION_ARGS)
{
	Datum		arg1;
	Datum		arg2;
	text	   *targ1;
	int64		pos64;
	int32		iarg3;
	text	   *slice;
	int32		ret;

	if (mys_is_mysql_session())
		return mys_string_locate(fcinfo);

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();
	if (PG_ARGISNULL(2))
		PG_RETURN_INT32(0);

	pos64 = PG_GETARG_INT64(2);
	if (pos64 <= 0 || pos64 > PG_INT32_MAX)
		PG_RETURN_INT32(0);

	iarg3 = (int32) pos64;
	arg1 = PG_GETARG_DATUM(0);
	arg2 = PG_GETARG_DATUM(1);
	targ1 = DatumGetTextPP(arg1);

	slice = text_substring(arg2, iarg3, -1, true);
	ret = (int32) text_position(slice, targ1, PG_GET_COLLATION());
	pfree(slice);
	if ((Pointer) targ1 != DatumGetPointer(arg1))
		pfree(targ1);

	if (ret > 0)
		PG_RETURN_INT32(iarg3 - 1 + ret);
	PG_RETURN_INT32(0);
}

PG_FUNCTION_INFO_V1(convertTextToDouble);
Datum
convertTextToDouble(PG_FUNCTION_ARGS)
{
	char	   *orig_str;
	char	   *endptr;
	int			len;
	float8		ret;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	orig_str = TextDatumGetCString(PG_GETARG_DATUM(0));
	len = strlen(orig_str);
	while (len > 0 && orig_str[len - 1] == ' ')
		orig_str[--len] = '\0';

	if (len == 0)
	{
		pfree(orig_str);
		PG_RETURN_FLOAT8(0.0);
	}

	ret = strtod(orig_str, &endptr);
	if (*endptr != '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("invalid input syntax for type %s: \"%s\"",
						"double precision", orig_str)));

	pfree(orig_str);
	PG_RETURN_FLOAT8(ret);
}

PG_FUNCTION_INFO_V1(convertTextToInt8);
Datum
convertTextToInt8(PG_FUNCTION_ARGS)
{
	char	   *str;
	int			len;
	int64		ret;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	str = TextDatumGetCString(PG_GETARG_DATUM(0));
	len = strlen(str);
	while (len > 0 && str[len - 1] == ' ')
		str[--len] = '\0';

	if (len == 0)
	{
		pfree(str);
		PG_RETURN_INT64(0);
	}

	errno = 0;
	ret = strtoll(str, NULL, 10);
	pfree(str);
	PG_RETURN_INT64(ret);
}

PG_FUNCTION_INFO_V1(convert_text_to_bigint_for_mysql);
Datum
convert_text_to_bigint_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *str;
	int			len;
	int			i = 0;
	bool		isNegative = false;
	uint64		uval = 0;
	uint64		max_abs;
	int			intDigits = 0;
	int			fracDigits = 0;
	char		firstFrac = '0';
	int64		ret;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	str = TextDatumGetCString(PG_GETARG_DATUM(0));
	len = strlen(str);

	while (i < len && str[i] == ' ')
		i++;

	if (i < len && (str[i] == '+' || str[i] == '-'))
	{
		isNegative = (str[i] == '-');
		i++;
	}

	max_abs = isNegative ? ((uint64) PG_INT64_MAX + 1) : (uint64) PG_INT64_MAX;

	while (i < len && isdigit((unsigned char) str[i]))
	{
		uint64		d = (uint64) (str[i] - '0');

		if (uval > (max_abs - d) / 10)
		{
			pfree(str);
			PG_RETURN_NULL();
		}
		uval = uval * 10 + d;
		intDigits++;
		i++;
	}

	if (i < len && str[i] == '.')
	{
		i++;
		while (i < len && isdigit((unsigned char) str[i]))
		{
			if (fracDigits == 0)
				firstFrac = str[i];
			fracDigits++;
			i++;
		}
	}

	while (i < len && str[i] == ' ')
		i++;

	if (i != len || (intDigits + fracDigits) == 0)
	{
		pfree(str);
		PG_RETURN_NULL();
	}

	pfree(str);

	if (firstFrac >= '5')
	{
		if (uval >= max_abs)
			PG_RETURN_NULL();
		uval++;
	}

	if (!isNegative)
		ret = (int64) uval;
	else if (uval == 0)
		ret = 0;
	else if (uval == (uint64) PG_INT64_MAX + 1)
		ret = PG_INT64_MIN;
	else
		ret = -((int64) uval);

	PG_RETURN_INT64(ret);
}

static bool
mys_parse_bitstr_to_uint64(const char *bitData, int bitDataLen, uint64 max_val, uint64 *out)
{
	uint64		acc = 0;

	for (int i = 0; i < bitDataLen; i++)
	{
		uint64		bit = (bitData[i] == '1') ? 1 : 0;

		if (acc > (max_val - bit) / 2)
			return false;
		acc = (acc << 1) | bit;
	}
	*out = acc;
	return true;
}

static bool
mys_varbit_to_uint64(const VarBit *vb, uint64 max_val, uint64 *out)
{
	int			nbits = VARBITLEN(vb);
	const bits8 *src = VARBITS(vb);
	uint64		acc = 0;

	for (int i = 0; i < nbits; i++)
	{
		uint64		bit = ((src[i / BITS_PER_BYTE] >> (BITS_PER_BYTE - 1 - (i % BITS_PER_BYTE))) & 1);

		if (acc > (max_val - bit) / 2)
			return false;
		acc = (acc << 1) | bit;
	}
	*out = acc;
	return true;
}

PG_FUNCTION_INFO_V1(convertTextVarbitToBigint);
Datum
convertTextVarbitToBigint(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	uint64		val = 0;
	bool		ok;

	ok = mys_parse_bitstr_to_uint64(VARDATA_ANY(txt),
									VARSIZE_ANY_EXHDR(txt),
									(uint64) PG_INT64_MAX,
									&val);
	PG_FREE_IF_COPY(txt, 0);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) val);
}

PG_FUNCTION_INFO_V1(convertVarbitToBigint);
Datum
convertVarbitToBigint(PG_FUNCTION_ARGS)
{
	VarBit	   *vb = PG_GETARG_VARBIT_P(0);
	uint64		val = 0;
	bool		ok = mys_varbit_to_uint64(vb, (uint64) PG_INT64_MAX, &val);

	PG_FREE_IF_COPY(vb, 0);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT64((int64) val);
}

PG_FUNCTION_INFO_V1(convertVarbitToInt);
Datum
convertVarbitToInt(PG_FUNCTION_ARGS)
{
	VarBit	   *vb = PG_GETARG_VARBIT_P(0);
	uint64		val = 0;
	bool		ok = mys_varbit_to_uint64(vb, (uint64) PG_INT32_MAX, &val);

	PG_FREE_IF_COPY(vb, 0);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT32((int32) val);
}

PG_FUNCTION_INFO_V1(convertVarbitToSmallint);
Datum
convertVarbitToSmallint(PG_FUNCTION_ARGS)
{
	VarBit	   *vb = PG_GETARG_VARBIT_P(0);
	uint64		val = 0;
	bool		ok = mys_varbit_to_uint64(vb, (uint64) PG_INT16_MAX, &val);

	PG_FREE_IF_COPY(vb, 0);
	if (!ok)
		PG_RETURN_NULL();
	PG_RETURN_INT16((int16) val);
}

static VarBit *
mys_int64_to_varbit(int64 val)
{
	uint64		uval = (uint64) val;
	int32		nbits;
	int32		rlen;
	int32		bitsleft;
	VarBit	   *ret;
	bits8	   *r;

	if (val < 0)
		nbits = 64;
	else if (uval == 0)
		nbits = 1;
	else
		nbits = pg_leftmost_one_pos64(uval) + 1;

	rlen = VARBITTOTALLEN(nbits);
	ret = (VarBit *) palloc0(rlen);
	SET_VARSIZE(ret, rlen);
	VARBITLEN(ret) = nbits;
	r = VARBITS(ret);
	bitsleft = nbits;

	while (bitsleft >= 8)
	{
		*r++ = (bits8) ((uval >> (bitsleft - 8)) & BITMASK);
		bitsleft -= 8;
	}
	if (bitsleft > 0)
		*r = (bits8) ((uval << (8 - bitsleft)) & BITMASK);

	return ret;
}

PG_FUNCTION_INFO_V1(convertSmallintToVarbit);
Datum
convertSmallintToVarbit(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(mys_int64_to_varbit((int64) PG_GETARG_INT16(0)));
}

PG_FUNCTION_INFO_V1(convertIntToVarbit);
Datum
convertIntToVarbit(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(mys_int64_to_varbit((int64) PG_GETARG_INT32(0)));
}

PG_FUNCTION_INFO_V1(convertBigintToVarbit);
Datum
convertBigintToVarbit(PG_FUNCTION_ARGS)
{
	PG_RETURN_VARBIT_P(mys_int64_to_varbit(PG_GETARG_INT64(0)));
}

PG_FUNCTION_INFO_V1(convert_text_to_digit_text_for_mysql);
Datum
convert_text_to_digit_text_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *orig_str;
	char	   *str;
	int			len;
	text	   *res;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	orig_str = TextDatumGetCString(PG_GETARG_DATUM(0));
	str = orig_str;
	len = strlen(str);

	if (mys_is_mysql_session())
	{
		char	   *end;

		(void) strtod(str, &end);
		if (end == str)
			mys_add_warning(1292, "Truncated incorrect DOUBLE value");
		else
		{
			while (isspace((unsigned char) *end))
				end++;
			if (*end)
				mys_add_warning(1292, "Truncated incorrect DOUBLE value");
		}
	}

	if (len >= 1)
	{
		convert_to_digit_text_for_mysql(&str, len);
		res = cstring_to_text(str);
	}
	else
	{
		res = cstring_to_text("0");
	}

	pfree(orig_str);
	PG_RETURN_TEXT_P(res);
}

PG_FUNCTION_INFO_V1(convert_digit_text_to_text_for_mysql);
Datum
convert_digit_text_to_text_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *orig_str;
	char	   *str;
	int			len;
	text	   *res;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	orig_str = TextDatumGetCString(PG_GETARG_DATUM(0));
	str = orig_str;
	len = strlen(str);

	convert_to_text_for_mysql(&str, len);
	res = cstring_to_text(str);
	pfree(orig_str);
	PG_RETURN_TEXT_P(res);
}

PG_FUNCTION_INFO_V1(ceil_for_mysql);
Datum
ceil_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *orig_str;
	char	   *str;
	int			len;
	float8		tmp;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	orig_str = TextDatumGetCString(PG_GETARG_DATUM(0));
	str = orig_str;
	len = strlen(str);
	if (len < 1)
	{
		pfree(orig_str);
		PG_RETURN_INT64(0);
	}

	convert_to_digit_text_for_mysql(&str, len);
	tmp = ceil(strtod(str, NULL));
	pfree(orig_str);

	if (isnan(tmp) || tmp < (float8) PG_INT64_MIN || tmp >= 9223372036854775808.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) tmp);
}

PG_FUNCTION_INFO_V1(floor_for_mysql);
Datum
floor_for_mysql(PG_FUNCTION_ARGS)
{
	char	   *orig_str;
	char	   *str;
	int			len;
	float8		tmp;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	orig_str = TextDatumGetCString(PG_GETARG_DATUM(0));
	str = orig_str;
	len = strlen(str);
	if (len < 1)
	{
		pfree(orig_str);
		PG_RETURN_INT64(0);
	}

	convert_to_digit_text_for_mysql(&str, len);
	tmp = floor(strtod(str, NULL));
	pfree(orig_str);

	if (isnan(tmp) || tmp < (float8) PG_INT64_MIN || tmp >= 9223372036854775808.0)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	PG_RETURN_INT64((int64) tmp);
}

PG_FUNCTION_INFO_V1(mys_bit_to_text);
Datum
mys_bit_to_text(PG_FUNCTION_ARGS)
{
	VarBit	   *s;
	int			bitLen;
	int			nbytes;
	int			shift;
	const bits8 *src;
	text	   *result;
	bits8	   *dst;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	s = PG_GETARG_VARBIT_P(0);
	bitLen = VARBITLEN(s);
	nbytes = VARBITBYTES(s);
	shift = nbytes * BITS_PER_BYTE - bitLen;
	src = VARBITS(s);
	result = (text *) palloc0(VARHDRSZ + nbytes);
	dst = (bits8 *) VARDATA(result);

	SET_VARSIZE(result, VARHDRSZ + nbytes);
	if (bitLen != 0)
	{
		if (shift == 0)
			memcpy(dst, src, nbytes);
		else
		{
			bits8		carry = 0;

			for (int i = 0; i < nbytes; i++)
			{
				dst[i] = carry | (src[i] >> shift);
				carry = (bits8) (src[i] << (BITS_PER_BYTE - shift));
			}
		}

		for (int i = 0; i < nbytes; i++)
		{
			if (dst[i] == '\0' || IS_HIGHBIT_SET(dst[i]))
			{
				pg_verifymbstr((const char *) dst, nbytes, false);
				break;
			}
		}
	}

	PG_FREE_IF_COPY(s, 0);
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(cast_bittext_to_bytea);
Datum
cast_bittext_to_bytea(PG_FUNCTION_ARGS)
{
	VarBit	   *s = PG_GETARG_VARBIT_P(0);
	int			nbytes = VARBITBYTES(s);
	int			shift = nbytes * BITS_PER_BYTE - VARBITLEN(s);
	const bits8 *src = VARBITS(s);
	bytea	   *result = palloc(VARHDRSZ + nbytes);
	bits8	   *dst = (bits8 *) VARDATA(result);

	SET_VARSIZE(result, VARHDRSZ + nbytes);
	if (shift == 0)
		memcpy(dst, src, nbytes);
	else
	{
		bits8		carry = 0;

		for (int i = 0; i < nbytes; i++)
		{
			dst[i] = carry | (src[i] >> shift);
			carry = (bits8) (src[i] << (BITS_PER_BYTE - shift));
		}
	}

	PG_FREE_IF_COPY(s, 0);
	PG_RETURN_BYTEA_P(result);
}

static text *
text_substring(Datum str, int32 start, int32 length, bool length_not_specified)
{
	int32		eml = pg_database_encoding_max_length();
	int32		S = start;
	int32		S1;
	int32		L1;
	int32		E;
	int			len;

	len = DatumGetInt32(DirectFunctionCall1(textlen, str));

	if (start == 0 || (start < 0 && (int64) len < -(int64) start))
		return cstring_to_text("");
	if (!length_not_specified && length <= 0)
		return cstring_to_text("");

	if (start < 0)
		S = len + 1 + start;

	S1 = Max(S, 1);

	if (eml == 1)
	{
		if (length_not_specified)
			L1 = -1;
		else if (length < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			L1 = -1;
		}
		else if (pg_add_s32_overflow(S, length, &E))
			L1 = -1;
		else
		{
			if (E < 1)
				return cstring_to_text("");
			L1 = E - S1;
		}

		return DatumGetTextPSlice(str, S1 - 1, L1);
	}
	else if (eml > 1)
	{
		int32		slice_start = 0;
		int32		slice_size;
		int32		slice_strlen;
		text	   *slice;
		int32		E1;
		int32		i;
		char	   *p;
		char	   *s;
		text	   *ret;

		if (length_not_specified)
			slice_size = L1 = -1;
		else if (length < 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SUBSTRING_ERROR),
					 errmsg("negative substring length not allowed")));
			slice_size = L1 = -1;
		}
		else if (pg_add_s32_overflow(S, length, &E))
			slice_size = L1 = -1;
		else
		{
			if (E < 1)
				return cstring_to_text("");
			L1 = E - S1;
			if (pg_mul_s32_overflow(E, eml, &slice_size))
				slice_size = -1;
		}

		if (VARATT_IS_COMPRESSED(DatumGetPointer(str)) ||
			VARATT_IS_EXTERNAL(DatumGetPointer(str)))
			slice = DatumGetTextPSlice(str, slice_start, slice_size);
		else
			slice = (text *) DatumGetPointer(str);

		if (VARSIZE_ANY_EXHDR(slice) == 0)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		slice_strlen = pg_mbstrlen_with_len(VARDATA_ANY(slice),
											VARSIZE_ANY_EXHDR(slice));

		if (S1 > slice_strlen)
		{
			if (slice != (text *) DatumGetPointer(str))
				pfree(slice);
			return cstring_to_text("");
		}

		if (L1 > -1)
			E1 = Min(S1 + L1, slice_start + 1 + slice_strlen);
		else
			E1 = slice_start + 1 + slice_strlen;

		p = VARDATA_ANY(slice);
		for (i = 0; i < S1 - 1; i++)
			p += pg_mblen(p);

		s = p;
		for (i = S1; i < E1; i++)
			p += pg_mblen(p);

		ret = (text *) palloc(VARHDRSZ + (p - s));
		SET_VARSIZE(ret, VARHDRSZ + (p - s));
		memcpy(VARDATA(ret), s, (p - s));

		if (slice != (text *) DatumGetPointer(str))
			pfree(slice);

		return ret;
	}
	else
		elog(ERROR, "invalid backend encoding: encoding max length < 1");

	return NULL;
}

PG_FUNCTION_INFO_V1(text_concat);
Datum
text_concat(PG_FUNCTION_ARGS)
{
	text	   *result = concat_internal("", 0, fcinfo, false);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(text_concat_ws);
Datum
text_concat_ws(PG_FUNCTION_ARGS)
{
	text	   *sep_txt;
	char	   *sep;
	text	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	sep_txt = PG_GETARG_TEXT_PP(0);
	sep = text_to_cstring(sep_txt);
	PG_FREE_IF_COPY(sep_txt, 0);

	result = concat_internal(sep, 1, fcinfo, true);
	pfree(sep);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

static text *
concat_internal(const char *sepstr, int argidx, FunctionCallInfo fcinfo, bool skip_nulls)
{
	text	   *result;
	StringInfoData str;
	FmgrInfo   *foutcache;
	bool		first_arg = true;
	int			i;

	initStringInfo(&str);

	foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
	if (foutcache == NULL)
		foutcache = build_concat_foutcache(fcinfo, argidx);

	for (i = argidx; i < PG_NARGS(); i++)
	{
		if (!PG_ARGISNULL(i))
		{
			Datum		value = PG_GETARG_DATUM(i);
			Oid			valtype = get_fn_expr_argtype(fcinfo->flinfo, i);
			Oid			valBaseType = mys_binary_base_type(valtype);

			if (first_arg)
				first_arg = false;
			else
				appendStringInfoString(&str, sepstr);

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
		else if (!skip_nulls)
		{
			pfree(str.data);
			return NULL;
		}
	}

	result = cstring_to_text_with_len(str.data, str.len);
	pfree(str.data);

	return result;
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
					 errmsg("could not determine data type of concat() input")));

		getTypeOutputInfo(valtype, &typOutput, &typIsVarlena);
		fmgr_info_cxt(typOutput, &foutcache[i], fcinfo->flinfo->fn_mcxt);
	}

	fcinfo->flinfo->fn_extra = foutcache;
	return foutcache;
}

static text *
ascii_lower_copy(const text *src)
{
	int			len = VARSIZE_ANY_EXHDR(src);
	const char *s = VARDATA_ANY(src);
	text	   *dst = (text *) palloc(VARHDRSZ + len);
	char	   *d = VARDATA(dst);

	SET_VARSIZE(dst, VARHDRSZ + len);
	for (int i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) s[i];

		d[i] = (c >= 'A' && c <= 'Z') ? (char) (c + ('a' - 'A')) : (char) c;
	}
	return dst;
}

static int
text_position(text *t1, text *t2, Oid collid)
{
	TextPositionState state;
	int			result;
	text	   *s1 = t1;
	text	   *s2 = t2;
	bool		copied = false;

	if (VARSIZE_ANY_EXHDR(t2) < 1)
		return 1;
	if (VARSIZE_ANY_EXHDR(t1) < VARSIZE_ANY_EXHDR(t2))
		return 0;

	if (collid == getCaseInsensitiveId())
	{
		s1 = ascii_lower_copy(t1);
		s2 = ascii_lower_copy(t2);
		copied = true;
		collid = C_COLLATION_OID;
	}

	text_position_setup(s1, s2, collid, &state);
	if (!text_position_next(collid, &state))
		result = 0;
	else
		result = text_position_get_match_pos(&state);

	if (copied)
	{
		pfree(s1);
		pfree(s2);
	}

	return result;
}

static void
text_position_setup(text *t1, text *t2, Oid collid, TextPositionState *state)
{
	int			len1 = VARSIZE_ANY_EXHDR(t1);
	int			len2 = VARSIZE_ANY_EXHDR(t2);
	pg_locale_t mylocale = NULL;

	check_collation_set_local(collid);

	if (!lc_collate_is_c(collid) && collid != DEFAULT_COLLATION_OID)
		mylocale = pg_newlocale_from_collation(collid);

	if (mylocale && !mylocale->deterministic)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("nondeterministic collations are not supported for substring searches")));

	Assert(len1 > 0);
	Assert(len2 > 0);

	if (pg_database_encoding_max_length() == 1)
	{
		state->is_multibyte = false;
		state->is_multibyte_char_in_char = false;
	}
	else if (GetDatabaseEncoding() == PG_UTF8)
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = false;
	}
	else
	{
		state->is_multibyte = true;
		state->is_multibyte_char_in_char = true;
	}

	state->str1 = VARDATA_ANY(t1);
	state->str2 = VARDATA_ANY(t2);
	state->len1 = len1;
	state->len2 = len2;
	state->last_match = NULL;
	state->refpoint = state->str1;
	state->refpos = 0;
	state->skiptablemask = 255;

	if (len1 >= len2 && len2 > 1)
	{
		int			searchlength = len1 - len2;
		int			skiptablemask;
		int			last;
		int			i;
		const char *str2 = state->str2;

		if (searchlength < 16)
			skiptablemask = 3;
		else if (searchlength < 64)
			skiptablemask = 7;
		else if (searchlength < 128)
			skiptablemask = 15;
		else if (searchlength < 512)
			skiptablemask = 31;
		else if (searchlength < 2048)
			skiptablemask = 63;
		else if (searchlength < 4096)
			skiptablemask = 127;
		else
			skiptablemask = 255;
		state->skiptablemask = skiptablemask;

		for (i = 0; i <= skiptablemask; i++)
			state->skiptable[i] = len2;

		last = len2 - 1;
		for (i = 0; i < last; i++)
			state->skiptable[(unsigned char) str2[i] & skiptablemask] = last - i;
	}
}

static bool
text_position_next(Oid collid, TextPositionState *state)
{
	int			needle_len = state->len2;
	char	   *start_ptr;
	char	   *matchptr;

	if (needle_len <= 0)
		return false;

	if (state->last_match)
		start_ptr = state->last_match + needle_len;
	else
		start_ptr = state->str1;

retry:
	matchptr = text_position_next_internal(collid, start_ptr, state);
	if (!matchptr)
		return false;

	if (state->is_multibyte_char_in_char)
	{
		Assert(state->refpoint <= matchptr);

		while (state->refpoint < matchptr)
		{
			state->refpoint += pg_mblen(state->refpoint);
			state->refpos++;

			if (state->refpoint > matchptr)
			{
				start_ptr = state->refpoint;
				goto retry;
			}
		}
	}

	state->last_match = matchptr;
	return true;
}

static char *
text_position_next_internal(Oid collid, char *start_ptr, TextPositionState *state)
{
	int			haystack_len = state->len1;
	int			needle_len = state->len2;
	int			skiptablemask = state->skiptablemask;
	const char *haystack = state->str1;
	const char *needle = state->str2;
	const char *haystack_end = &haystack[haystack_len];
	const char *hptr;

	Assert(start_ptr >= haystack && start_ptr <= haystack_end);

	if (needle_len == 1)
	{
		char		nchar = *needle;

		for (hptr = start_ptr; hptr < haystack_end; hptr++)
		{
			if (*hptr == nchar)
				return (char *) hptr;
		}
	}
	else
	{
		const char *needle_last = &needle[needle_len - 1];

		hptr = start_ptr + needle_len - 1;
		while (hptr < haystack_end)
		{
			const char *nptr = needle_last;
			const char *p = hptr;

			while (*nptr == *p)
			{
				if (nptr == needle)
					return (char *) p;
				nptr--, p--;
			}

			hptr += state->skiptable[(unsigned char) *hptr & skiptablemask];
		}
	}

	return NULL;
}

static int
text_position_get_match_pos(TextPositionState *state)
{
	if (!state->is_multibyte)
		return state->last_match - state->str1 + 1;

	while (state->refpoint < state->last_match)
	{
		state->refpoint += pg_mblen(state->refpoint);
		state->refpos++;
	}
	Assert(state->refpoint == state->last_match);
	return state->refpos + 1;
}

static void
check_collation_set_local(Oid collid)
{
	if (!OidIsValid(collid))
		ereport(ERROR,
				(errcode(ERRCODE_INDETERMINATE_COLLATION),
				 errmsg("could not determine which collation to use for string comparison"),
				 errhint("Use the COLLATE clause to set the collation explicitly.")));
}

static void
convert_to_digit_text_for_mysql(char **str, int str_len)
{
	char	   *ptr = *str;
	int			dotNum = 0;
	int			i = 0;

	if (str_len <= 0)
		return;

	while (i < str_len && ptr[i] == ' ')
		i++;
	if (i > 0)
	{
		ptr += i;
		str_len -= i;
	}

	if (str_len <= 0)
	{
		(*str)[0] = '0';
		(*str)[1] = '\0';
		return;
	}

	if (isdigit((unsigned char) ptr[0]) || ptr[0] == '+')
	{
		if (ptr[0] == '+')
		{
			ptr++;
			str_len--;
		}
		i = 0;
		while (i < str_len && ptr[i] == '0')
			i++;
		if (i > 0)
		{
			ptr += i;
			str_len -= i;
		}

		for (i = 0; i < str_len; i++)
		{
			if (!isdigit((unsigned char) ptr[i]))
			{
				if (ptr[i] == '.')
				{
					if (++dotNum > 1)
					{
						ptr[i] = '\0';
						break;
					}
				}
				else
				{
					ptr[i] = '\0';
					break;
				}
			}
		}

		if (i <= 0 || (i == 1 && ptr[0] == '.'))
		{
			(*str)[0] = '0';
			(*str)[1] = '\0';
		}
		else
			*str = ptr;
	}
	else if (ptr[0] == '-')
	{
		for (i = 1; i < str_len; i++)
		{
			if (ptr[i] != '0')
				break;
		}
		if (i <= 1)
		{
			for (i = 1; i < str_len; i++)
			{
				if (!isdigit((unsigned char) ptr[i]))
				{
					if (ptr[i] == '.')
					{
						if (++dotNum > 1)
						{
							ptr[i] = '\0';
							break;
						}
					}
					else
					{
						ptr[i] = '\0';
						break;
					}
				}
			}
			if (i > 1 && !(i == 2 && ptr[1] == '.'))
				*str = ptr;
			else
			{
				(*str)[0] = '0';
				(*str)[1] = '\0';
			}
		}
		else
		{
			int			skip = i;

			ptr += skip;
			str_len -= skip;
			for (i = 0; i < str_len; i++)
			{
				if (!isdigit((unsigned char) ptr[i]))
				{
					if (ptr[i] == '.')
					{
						if (++dotNum > 1)
						{
							ptr[i] = '\0';
							break;
						}
					}
					else
					{
						ptr[i] = '\0';
						break;
					}
				}
			}
			if (i > 0 && !(i == 1 && ptr[0] == '.'))
			{
				int			j = 0;

				(*str)[j++] = '-';
				for (int k = 0; k < i; k++)
					(*str)[j++] = ptr[k];
				(*str)[j] = '\0';
			}
			else
			{
				(*str)[0] = '0';
				(*str)[1] = '\0';
			}
		}
	}
	else if (ptr[0] == '.')
	{
		for (i = 1; i < str_len; i++)
		{
			if (!isdigit((unsigned char) ptr[i]))
			{
				ptr[i] = '\0';
				break;
			}
		}
		if (i <= 1)
		{
			(*str)[0] = '0';
			(*str)[1] = '\0';
		}
		else
			*str = ptr;
	}
	else
	{
		(*str)[0] = '0';
		(*str)[1] = '\0';
	}
}

static void
convert_to_text_for_mysql(char **str, int str_len)
{
	char	   *ptr = *str;
	int			i = 0;

	if (str_len <= 0)
		return;

	if (isdigit((unsigned char) ptr[0]) || ptr[0] == '+')
	{
		if (ptr[0] == '+')
		{
			ptr++;
			str_len--;
		}
		while (i < str_len && ptr[i] == '0')
			i++;
		ptr += i;
		str_len -= i;

		if (str_len > 0)
			*str = ptr;
		else
		{
			(*str)[0] = '0';
			(*str)[1] = '\0';
		}
	}
	else if (ptr[0] == '-')
	{
		for (i = 1; i < str_len; i++)
		{
			if (ptr[i] != '0')
				break;
		}
		if (i == 1)
		{
			if (str_len > 1)
				*str = ptr;
			else
			{
				(*str)[0] = '0';
				(*str)[1] = '\0';
			}
		}
		else
		{
			ptr += i;
			str_len -= i;
			if (str_len > 0)
			{
				int			j = 0;

				(*str)[j++] = '-';
				for (i = 0; i < str_len; i++)
					(*str)[j++] = ptr[i];
				(*str)[j] = '\0';
			}
			else
			{
				(*str)[0] = '0';
				(*str)[1] = '\0';
			}
		}
	}
	else
	{
		(*str)[0] = '0';
		(*str)[1] = '\0';
	}
}

PG_FUNCTION_INFO_V1(rowCount);
Datum
rowCount(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(affectedRows);
}

PG_FUNCTION_INFO_V1(mysFoundRows);
Datum
mysFoundRows(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(foundRows);
}

PG_FUNCTION_INFO_V1(mysLastInsertId);
Datum
mysLastInsertId(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT64(lastInsertID);
}

PG_FUNCTION_INFO_V1(substringIndex);
Datum
substringIndex(PG_FUNCTION_ARGS)
{
	text	   *str_txt = PG_GETARG_TEXT_PP(0);
	text	   *sep_txt = PG_GETARG_TEXT_PP(1);
	int32		index = PG_GETARG_INT32(2);
	const char *str = VARDATA_ANY(str_txt);
	int			strLen = VARSIZE_ANY_EXHDR(str_txt);
	const char *sep = VARDATA_ANY(sep_txt);
	int			sepLen = VARSIZE_ANY_EXHDR(sep_txt);
	const char *res_ptr = str;
	int			res_len = 0;
	text	   *result;

	if (sepLen == 0 || index == 0 || strLen == 0)
	{
		res_len = 0;
	}
	else if (index > 0)
	{
		res_ptr = str;
		res_len = strLen;
		for (int i = 0; i <= strLen - sepLen; i++)
		{
			if (memcmp(str + i, sep, sepLen) == 0)
			{
				index--;
				if (index == 0)
				{
					res_len = i;
					break;
				}
				i += sepLen - 1;
			}
		}
	}
	else
	{
		res_ptr = str;
		res_len = strLen;
		for (int i = strLen - sepLen; i >= 0; i--)
		{
			if (memcmp(str + i, sep, sepLen) == 0)
			{
				index++;
				if (index == 0)
				{
					res_ptr = str + i + sepLen;
					res_len = strLen - (i + sepLen);
					break;
				}
				i -= sepLen - 1;
			}
		}
	}

	result = cstring_to_text_with_len(res_ptr, res_len);
	PG_FREE_IF_COPY(str_txt, 0);
	PG_FREE_IF_COPY(sep_txt, 1);
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(mysVersion);
Datum
mysVersion(PG_FUNCTION_ARGS)
{
	const char *ver = (halo_mysql_version != NULL) ? halo_mysql_version : "8.0.36-openHalo";

	PG_RETURN_TEXT_P(cstring_to_text(ver));
}

PG_FUNCTION_INFO_V1(isValidBinary);
Datum
isValidBinary(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	const char *str = VARDATA_ANY(txt);
	int			strLen = VARSIZE_ANY_EXHDR(txt);
	bool		valid = true;

	for (int i = 0; i < strLen; i++)
	{
		if (str[i] != '0' && str[i] != '1')
		{
			valid = false;
			break;
		}
	}

	PG_FREE_IF_COPY(txt, 0);
	PG_RETURN_BOOL(valid);
}

PG_FUNCTION_INFO_V1(isValidHex);
Datum
isValidHex(PG_FUNCTION_ARGS)
{
	text	   *txt = PG_GETARG_TEXT_PP(0);
	const char *str = VARDATA_ANY(txt);
	int			strLen = VARSIZE_ANY_EXHDR(txt);
	bool		valid = true;

	for (int i = 0; i < strLen; i++)
	{
		if (!isxdigit((unsigned char) str[i]))
		{
			valid = false;
			break;
		}
	}

	PG_FREE_IF_COPY(txt, 0);
	PG_RETURN_BOOL(valid);
}

PG_FUNCTION_INFO_V1(convertTextHexToTextVarbit);
Datum
convertTextHexToTextVarbit(PG_FUNCTION_ARGS)
{
	static const char hex_bits[16][5] = {
		"0000", "0001", "0010", "0011",
		"0100", "0101", "0110", "0111",
		"1000", "1001", "1010", "1011",
		"1100", "1101", "1110", "1111"
	};
	text	   *txt = PG_GETARG_TEXT_PP(0);
	const char *data = VARDATA_ANY(txt);
	int			dataLen = VARSIZE_ANY_EXHDR(txt);
	char	   *buf;
	int			j = 0;
	int			start = 0;
	text	   *res;

	if (dataLen == 0)
	{
		PG_FREE_IF_COPY(txt, 0);
		PG_RETURN_TEXT_P(cstring_to_text(""));
	}

	if (dataLen > 16)
	{
		PG_FREE_IF_COPY(txt, 0);
		PG_RETURN_TEXT_P(cstring_to_text("1111111111111111111111111111111111111111111111111111111111111111"));
	}

	buf = palloc(dataLen * 4 + 1);
	for (int i = 0; i < dataLen; i++)
	{
		unsigned char c = (unsigned char) data[i];
		int			nibble;

		if (c >= '0' && c <= '9')
			nibble = c - '0';
		else if (c >= 'a' && c <= 'f')
			nibble = 10 + (c - 'a');
		else if (c >= 'A' && c <= 'F')
			nibble = 10 + (c - 'A');
		else
			nibble = 15;

		memcpy(buf + j, hex_bits[nibble], 4);
		j += 4;
	}
	buf[j] = '\0';
	PG_FREE_IF_COPY(txt, 0);

	while (start < j - 1 && buf[start] == '0')
		start++;

	res = cstring_to_text(buf + start);
	pfree(buf);
	PG_RETURN_TEXT_P(res);
}
