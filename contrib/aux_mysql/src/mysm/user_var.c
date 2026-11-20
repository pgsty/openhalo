/*-------------------------------------------------------------------------
 *
 * user_var.c
 *	  MySQL user variable (@var) casts, comparisons, and operators
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

#include "adapter/mysql/adapter.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "commands/mysql/mys_uservar.h"
#include "fmgr.h"
#include "mb/pg_wchar.h"
#include "nodes/nodes.h"
#include "utils/builtins.h"
#include "utils/bytea.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"
#include "utils/varbit.h"

typedef enum MysCmpOp
{
	MYS_CMP_EQ = 0,
	MYS_CMP_NE,
	MYS_CMP_LT,
	MYS_CMP_LE,
	MYS_CMP_GT,
	MYS_CMP_GE
} MysCmpOp;

static Oid	getParameterType(FmgrInfo *fmgrinfo, int parmIndex);
static bool evalCmpResult(int32 cmp, MysCmpOp op);
static Datum anycompatibleCmpBytea(Oid parm0Type, Datum parm0Val, Datum parm1Val,
								   MysCmpOp op, Oid collid);
static Datum byteaCmpBytea(Datum d0, Datum d1, MysCmpOp op);
static Datum convertParameterToText(Oid parmType, Datum parmVal);
static Datum convertParameterToNumeric(Oid parmType, Datum parm);
static Datum convertParameterToInt8(Oid parmType, Datum parm);
static Datum bytea2int8_internal(bytea *vlena);
static Datum bytea2numeric_internal(bytea *vlena);
static char *AppendSeconds(char *cp, int sec, fsec_t fsec, int precision, bool fillzeros);
static void format_time_numeric_str(TimeADT timeADT, char *out, size_t outsz);
static void format_date_numeric_str(DateADT dateADT, char *out);
static void format_timestamp_numeric_str(Timestamp timestamp, int *tzp, char *out);
static void truncateStrTailWhiteSpacesForDigit(size_t strLen, char *str);
static Datum bytea_to_valid_text_datum(Datum d);
static Oid	resolve_like_collation(Oid call_collid, PGFunction *like_fn, bool negate);

/* ----------------------------------------------------------------
 *	Temporal -> float8 helpers
 * ----------------------------------------------------------------
 */

static void
format_time_numeric_str(TimeADT timeADT, char *out, size_t outsz)
{
	struct pg_tm tt;
	fsec_t		fsec;
	char		buf[MAXDATELEN + 1];
	char	   *strp = buf;
	bool		nega = false;

	if (timeADT < 0)
	{
		nega = true;
		/* Avoid signed overflow on PG_INT64_MIN */
		timeADT = (timeADT == PG_INT64_MIN) ? PG_INT64_MAX : -timeADT;
	}

	time2tm(timeADT, &tt, &fsec);
	strp = pg_ultostr_zeropad(strp, tt.tm_hour, 2);
	strp = pg_ultostr_zeropad(strp, tt.tm_min, 2);
	strp = AppendSeconds(strp, tt.tm_sec, fsec, MAX_TIME_PRECISION, true);
	*strp = '\0';

	pg_snprintf(out, outsz, "%s%s", nega ? "-" : "", buf);
}

static void
format_date_numeric_str(DateADT dateADT, char *out)
{
	struct pg_tm tt;
	char	   *strp = out;

	if (DATE_NOT_FINITE(dateADT))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("date out of range")));

	j2date(dateADT + POSTGRES_EPOCH_JDATE,
		   &tt.tm_year, &tt.tm_mon, &tt.tm_mday);
	strp = pg_ultostr_zeropad(strp,
							  (tt.tm_year > 0) ? tt.tm_year : -(tt.tm_year - 1),
							  4);
	strp = pg_ultostr_zeropad(strp, tt.tm_mon, 2);
	strp = pg_ultostr_zeropad(strp, tt.tm_mday, 2);
	*strp = '\0';
}

static void
format_timestamp_numeric_str(Timestamp timestamp, int *tzp, char *out)
{
	struct pg_tm tt;
	fsec_t		fsec;
	const char *tzn = NULL;

	if (TIMESTAMP_NOT_FINITE(timestamp) ||
		timestamp2tm(timestamp, tzp, &tt, &fsec, tzp ? &tzn : NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	out = pg_ultostr_zeropad(out,
							 (tt.tm_year > 0) ? tt.tm_year : -(tt.tm_year - 1),
							 4);
	out = pg_ultostr_zeropad(out, tt.tm_mon, 2);
	out = pg_ultostr_zeropad(out, tt.tm_mday, 2);
	out = pg_ultostr_zeropad(out, tt.tm_hour, 2);
	out = pg_ultostr_zeropad(out, tt.tm_min, 2);
	out = AppendSeconds(out, tt.tm_sec, fsec, MAX_TIMESTAMP_PRECISION, true);
	*out = '\0';
}

PG_FUNCTION_INFO_V1(convertTimeToDouble);
Datum
convertTimeToDouble(PG_FUNCTION_ARGS)
{
	TimeADT		timeADT = DatumGetTimeADT(PG_GETARG_DATUM(0));
	char		parmStr[MAXDATELEN + 1];

	format_time_numeric_str(timeADT, parmStr, sizeof(parmStr));
	return DirectFunctionCall3(float8in,
							   CStringGetDatum(parmStr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

PG_FUNCTION_INFO_V1(convertDateToDouble);
Datum
convertDateToDouble(PG_FUNCTION_ARGS)
{
	DateADT		dateADT = DatumGetDateADT(PG_GETARG_DATUM(0));
	char		parmStr[MAXDATELEN + 1];

	format_date_numeric_str(dateADT, parmStr);
	return DirectFunctionCall3(float8in,
							   CStringGetDatum(parmStr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

PG_FUNCTION_INFO_V1(convertTimestampToDouble);
Datum
convertTimestampToDouble(PG_FUNCTION_ARGS)
{
	Timestamp	timestamp = DatumGetTimestamp(PG_GETARG_DATUM(0));
	char		parmStr[MAXDATELEN + 1];

	format_timestamp_numeric_str(timestamp, NULL, parmStr);
	return DirectFunctionCall3(float8in,
							   CStringGetDatum(parmStr),
							   ObjectIdGetDatum(InvalidOid),
							   Int32GetDatum(-1));
}

/* ----------------------------------------------------------------
 *	User variable SQL entry points
 * ----------------------------------------------------------------
 */

PG_FUNCTION_INFO_V1(mysSetUserVar);
Datum
mysSetUserVar(PG_FUNCTION_ARGS)
{
	char	   *varName;

	if (PG_ARGISNULL(0))
		PG_RETURN_VOID();

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));

	if (!PG_ARGISNULL(1))
	{
		Datum		datumValue = PG_GETARG_DATUM(1);
		Oid			varValueType = get_fn_expr_argtype(fcinfo->flinfo, 1);

		if (!OidIsValid(varValueType))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine data type of user variable input")));

		mysSetUserVarDatum(varName, datumValue, varValueType, NULL, false);
	}
	else
	{
		mysSetUserVarDatum(varName, (Datum) 0, InvalidOid, NULL, true);
	}

	pfree(varName);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(mysClearUserVariables);
Datum
mysClearUserVariables(PG_FUNCTION_ARGS)
{
	clearUserVars();
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(mysSetUserVarInExpr);
Datum
mysSetUserVarInExpr(PG_FUNCTION_ARGS)
{
	char	   *varName;
	bytea	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));

	if (!PG_ARGISNULL(1))
	{
		Datum		datumValue = PG_GETARG_DATUM(1);
		Oid			varValueType = get_fn_expr_argtype(fcinfo->flinfo, 1);
		Oid			baseType;
		FmgrInfo   *foutcache = NULL;

		if (!OidIsValid(varValueType))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("could not determine data type of user variable input")));

		baseType = mys_binary_base_type(varValueType);
		if (baseType != BYTEAOID &&
			getBaseType(varValueType) != BPCHAROID &&
			baseType != BITOID &&
			baseType != VARBITOID &&
			baseType != BOOLOID)
		{
			foutcache = (FmgrInfo *) fcinfo->flinfo->fn_extra;
			if (foutcache == NULL)
			{
				Oid			typOutput;
				bool		typIsVarlena;

				foutcache = (FmgrInfo *) MemoryContextAlloc(fcinfo->flinfo->fn_mcxt,
															sizeof(FmgrInfo));
				getTypeOutputInfo(varValueType, &typOutput, &typIsVarlena);
				fmgr_info_cxt(typOutput, foutcache, fcinfo->flinfo->fn_mcxt);
				fcinfo->flinfo->fn_extra = (void *) foutcache;
			}
		}

		mysSetUserVarDatum(varName, datumValue, varValueType, foutcache, false);
	}
	else
	{
		mysSetUserVarDatum(varName, (Datum) 0, InvalidOid, NULL, true);
	}

	result = mysGetUserVarValueInternal(varName);
	pfree(varName);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(result);
}

PG_FUNCTION_INFO_V1(mysGetUserVarValue);
Datum
mysGetUserVarValue(PG_FUNCTION_ARGS)
{
	char	   *varName;
	bytea	   *result;

	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));
	result = mysGetUserVarValueInternal(varName);
	pfree(varName);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_BYTEA_P(result);
}

/* ----------------------------------------------------------------
 *	bytea -> target type cast functions
 * ----------------------------------------------------------------
 */

static char *
bytea_to_checked_cstring(bytea *vlena, const char *typname)
{
	const char *byte = VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);

	if (memchr(byte, '\0', byteLen) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
				 errmsg("Invalid input syntax for type %s", typname)));

	return pnstrdup(byte, byteLen);
}

PG_FUNCTION_INFO_V1(mys_bytea2int8);
Datum
mys_bytea2int8(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "bigint");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall1(int8in, CStringGetDatum(str));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2int4);
Datum
mys_bytea2int4(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "integer");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall1(int4in, CStringGetDatum(str));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2int2);
Datum
mys_bytea2int2(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "smallint");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall1(int2in, CStringGetDatum(str));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2numeric);
Datum
mys_bytea2numeric(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "numeric");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall3(numeric_in,
								 CStringGetDatum(str),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(-1));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2float8);
Datum
mys_bytea2float8(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "double precision");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall1(float8in, CStringGetDatum(str));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2float4);
Datum
mys_bytea2float4(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "real");
	Datum		result;

	truncateStrTailWhiteSpacesForDigit(strlen(str), str);
	result = DirectFunctionCall1(float4in, CStringGetDatum(str));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2text);
Datum
mys_bytea2text(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	const char *byte = VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);

	pg_verifymbstr(byte, (int) byteLen, false);
	PG_RETURN_TEXT_P(cstring_to_text_with_len(byte, (int) byteLen));
}

PG_FUNCTION_INFO_V1(mys_bytea2varchar);
Datum
mys_bytea2varchar(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	const char *byte = VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);

	pg_verifymbstr(byte, (int) byteLen, false);
	PG_RETURN_VARCHAR_P((VarChar *) cstring_to_text_with_len(byte, (int) byteLen));
}

PG_FUNCTION_INFO_V1(mys_bytea2bpchar);
Datum
mys_bytea2bpchar(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	const char *byte = VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);
	char	   *str;
	Datum		result;

	pg_verifymbstr(byte, (int) byteLen, false);
	str = pnstrdup(byte, byteLen);
	result = DirectFunctionCall3(bpcharin,
								 CStringGetDatum(str),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(-1));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2date);
Datum
mys_bytea2date(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "date");
	Datum		result = DirectFunctionCall1(date_in, CStringGetDatum(str));

	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2time);
Datum
mys_bytea2time(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "time");
	Datum		result = DirectFunctionCall3(time_in,
											 CStringGetDatum(str),
											 ObjectIdGetDatum(InvalidOid),
											 Int32GetDatum(-1));

	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2timetz);
Datum
mys_bytea2timetz(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "time with time zone");
	Datum		result = DirectFunctionCall3(timetz_in,
											 CStringGetDatum(str),
											 ObjectIdGetDatum(InvalidOid),
											 Int32GetDatum(-1));

	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2timestamp);
Datum
mys_bytea2timestamp(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "timestamp");
	Datum		result = DirectFunctionCall3(timestamp_in,
											 CStringGetDatum(str),
											 ObjectIdGetDatum(InvalidOid),
											 Int32GetDatum(-1));

	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2timestamptz);
Datum
mys_bytea2timestamptz(PG_FUNCTION_ARGS)
{
	char	   *str = bytea_to_checked_cstring(PG_GETARG_BYTEA_PP(0), "timestamp with time zone");
	Datum		result = DirectFunctionCall3(timestamptz_in,
											 CStringGetDatum(str),
											 ObjectIdGetDatum(InvalidOid),
											 Int32GetDatum(-1));

	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2bool);
Datum
mys_bytea2bool(PG_FUNCTION_ARGS)
{
	bytea	   *vlena = PG_GETARG_BYTEA_PP(0);
	const char *byte = VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);
	size_t		index;
	bool		result = false;

	for (index = 0; index < byteLen; index++)
	{
		if (!isdigit((unsigned char) byte[index]))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
					 errmsg("Invalid input syntax for type boolean")));
		if (byte[index] != '0')
			result = true;
	}

	PG_RETURN_BOOL(result);
}

static Datum
bytea_to_bit_common(bytea *vlena, PGFunction target_in)
{
	const uint8 *byte = (const uint8 *) VARDATA_ANY(vlena);
	size_t		byteLen = VARSIZE_ANY_EXHDR(vlena);
	char	   *str = NULL;
	int			strIndex = 0;
	int			strLen = 0;
	size_t		pre_zero_bit_num = 0;
	bool		ok = false;
	size_t		index;
	int			i;
	Datum		result;

	for (index = 0; index < byteLen; index++)
	{
		for (i = 0; i < BITS_PER_BYTE; i++)
		{
			bool		curBitIsZero = ((byte[index] & (0x80 >> i)) == 0);

			if (ok)
			{
				str[strIndex++] = curBitIsZero ? '0' : '1';
			}
			else if (curBitIsZero)
			{
				pre_zero_bit_num++;
			}
			else
			{
				size_t		valid_bit_num = byteLen * BITS_PER_BYTE - pre_zero_bit_num;

				if (valid_bit_num > 64)
					ereport(ERROR,
							(errcode(ERRCODE_STRING_DATA_RIGHT_TRUNCATION),
							 errmsg("Data too long for type bit")));

				str = palloc(1 + valid_bit_num + 1);
				str[0] = 'b';
				str[1] = '1';
				strIndex = 2;
				strLen = (int) (1 + valid_bit_num);
				ok = true;
			}
		}
	}

	if (str == NULL)
		str = pstrdup("");
	else
		str[strLen] = '\0';

	result = DirectFunctionCall3(target_in,
								 CStringGetDatum(str),
								 ObjectIdGetDatum(InvalidOid),
								 Int32GetDatum(-1));
	pfree(str);
	return result;
}

PG_FUNCTION_INFO_V1(mys_bytea2bit);
Datum
mys_bytea2bit(PG_FUNCTION_ARGS)
{
	return bytea_to_bit_common(PG_GETARG_BYTEA_PP(0), bit_in);
}

PG_FUNCTION_INFO_V1(mys_bytea2varbit);
Datum
mys_bytea2varbit(PG_FUNCTION_ARGS)
{
	return bytea_to_bit_common(PG_GETARG_BYTEA_PP(0), varbit_in);
}

/* ----------------------------------------------------------------
 *	Comparison operators (anycompatible <-> bytea, bytea <-> bytea)
 * ----------------------------------------------------------------
 */

static bool
evalCmpResult(int32 cmp, MysCmpOp op)
{
	switch (op)
	{
		case MYS_CMP_EQ:
			return (cmp == 0);
		case MYS_CMP_NE:
			return (cmp != 0);
		case MYS_CMP_LT:
			return (cmp < 0);
		case MYS_CMP_LE:
			return (cmp <= 0);
		case MYS_CMP_GT:
			return (cmp > 0);
		case MYS_CMP_GE:
			return (cmp >= 0);
	}
	return false;
}

static MysCmpOp
commuteCmpOp(MysCmpOp op)
{
	switch (op)
	{
		case MYS_CMP_LT:
			return MYS_CMP_GT;
		case MYS_CMP_LE:
			return MYS_CMP_GE;
		case MYS_CMP_GT:
			return MYS_CMP_LT;
		case MYS_CMP_GE:
			return MYS_CMP_LE;
		default:
			return op;
	}
}

static Datum
anycompatibleCmpBytea(Oid parm0Type, Datum parm0Val, Datum parm1Val,
					  MysCmpOp op, Oid collid)
{
	Oid			effColl = OidIsValid(collid) ? collid : DEFAULT_COLLATION_OID;
	bool		parm0IsDigit = varValueIsDigit(parm0Val, parm0Type);
	bool		parm1IsDigit = varValueIsDigit(parm1Val, BYTEAOID);
	int32		cmp = 0;

	if (parm0Type == INT2OID ||
		parm0Type == INT4OID ||
		parm0Type == INT8OID ||
		parm0Type == NUMERICOID ||
		parm0Type == FLOAT4OID ||
		parm0Type == FLOAT8OID)
	{
		Datum		p0 = convertParameterToNumeric(parm0Type, parm0Val);
		Datum		p1 = convertParameterToNumeric(BYTEAOID, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));
	}
	else if (parm0Type == TEXTOID || parm0Type == VARCHAROID)
	{
		if (parm1IsDigit)
		{
			Datum		p0 = convertParameterToNumeric(parm0Type, parm0Val);
			Datum		p1 = convertParameterToNumeric(BYTEAOID, parm1Val);

			cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));
		}
		else
		{
			Datum		p1 = (parm0Type == TEXTOID) ?
				DirectFunctionCall1(mys_bytea2text, parm1Val) :
				DirectFunctionCall1(mys_bytea2varchar, parm1Val);

			if (op == MYS_CMP_EQ || op == MYS_CMP_NE)
			{
				bool		eq = DatumGetBool(DirectFunctionCall2Coll(texteq, effColl,
																	  parm0Val, p1));

				PG_RETURN_BOOL(op == MYS_CMP_EQ ? eq : !eq);
			}
			cmp = DatumGetInt32(DirectFunctionCall2Coll(bttextcmp, effColl,
														parm0Val, p1));
		}
	}
	else if (parm0Type == BPCHAROID)
	{
		if (parm1IsDigit)
		{
			Datum		p0 = convertParameterToNumeric(BPCHAROID, parm0Val);
			Datum		p1 = convertParameterToNumeric(BYTEAOID, parm1Val);

			cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));
		}
		else
		{
			Datum		p1 = DirectFunctionCall1(mys_bytea2bpchar, parm1Val);

			cmp = DatumGetInt32(DirectFunctionCall2Coll(bpcharcmp, effColl,
														parm0Val, p1));
		}
	}
	else if (parm0Type == DATEOID)
	{
		Datum		p1 = DirectFunctionCall1(mys_bytea2date, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(date_cmp, parm0Val, p1));
	}
	else if (parm0Type == TIMEOID)
	{
		Datum		p1 = DirectFunctionCall1(mys_bytea2time, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(time_cmp, parm0Val, p1));
	}
	else if (parm0Type == TIMETZOID)
	{
		Datum		p1 = DirectFunctionCall1(mys_bytea2timetz, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(timetz_cmp, parm0Val, p1));
	}
	else if (parm0Type == TIMESTAMPOID)
	{
		Datum		p1 = DirectFunctionCall1(mys_bytea2timestamp, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(timestamp_cmp, parm0Val, p1));
	}
	else if (parm0Type == TIMESTAMPTZOID)
	{
		Datum		p1 = DirectFunctionCall1(mys_bytea2timestamptz, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(timestamp_cmp, parm0Val, p1));
	}
	else if (parm0Type == BOOLOID)
	{
		Datum		p0 = convertParameterToNumeric(BOOLOID, parm0Val);
		Datum		p1 = convertParameterToNumeric(BYTEAOID, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));
	}
	else if (parm0Type == BITOID || parm0Type == VARBITOID)
	{
		Datum		p1 = (parm0Type == BITOID) ?
			DirectFunctionCall1(mys_bytea2bit, parm1Val) :
			DirectFunctionCall1(mys_bytea2varbit, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(bitcmp, parm0Val, p1));
	}
	else if (parm0IsDigit || parm1IsDigit)
	{
		Datum		p0 = convertParameterToNumeric(parm0Type, parm0Val);
		Datum		p1 = convertParameterToNumeric(BYTEAOID, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));
	}
	else
	{
		Datum		p0 = convertParameterToText(parm0Type, parm0Val);
		Datum		p1 = DirectFunctionCall1(mys_bytea2text, parm1Val);

		cmp = DatumGetInt32(DirectFunctionCall2Coll(bttextcmp, effColl, p0, p1));
	}

	PG_RETURN_BOOL(evalCmpResult(cmp, op));
}

static Datum
byteaCmpBytea(Datum d0, Datum d1, MysCmpOp op)
{
	if (varValueIsDigit(d0, BYTEAOID) || varValueIsDigit(d1, BYTEAOID))
	{
		Datum		p0 = convertParameterToNumeric(BYTEAOID, d0);
		Datum		p1 = convertParameterToNumeric(BYTEAOID, d1);
		int32		cmp = DatumGetInt32(DirectFunctionCall2(numeric_cmp, p0, p1));

		PG_RETURN_BOOL(evalCmpResult(cmp, op));
	}
	else
	{
		int32		cmp = DatumGetInt32(DirectFunctionCall2(byteacmp, d0, d1));

		PG_RETURN_BOOL(evalCmpResult(cmp, op));
	}
}

#define DEFINE_ANY_BYTEA_CMP_PAIR(fn_any_bytea, fn_bytea_any, op_enum) \
PG_FUNCTION_INFO_V1(fn_any_bytea); \
Datum \
fn_any_bytea(PG_FUNCTION_ARGS) \
{ \
	Oid parm0Type = getParameterType(fcinfo->flinfo, 0); \
	return anycompatibleCmpBytea(parm0Type, PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), \
								 (op_enum), PG_GET_COLLATION()); \
} \
PG_FUNCTION_INFO_V1(fn_bytea_any); \
Datum \
fn_bytea_any(PG_FUNCTION_ARGS) \
{ \
	Oid parm1Type = getParameterType(fcinfo->flinfo, 1); \
	return anycompatibleCmpBytea(parm1Type, PG_GETARG_DATUM(1), PG_GETARG_DATUM(0), \
								 commuteCmpOp(op_enum), PG_GET_COLLATION()); \
}

DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleEqByteaCImp, byteaEqAnycompatibleCImp, MYS_CMP_EQ)
DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleNeByteaCImp, byteaNeAnycompatibleCImp, MYS_CMP_NE)
DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleLtByteaCImp, byteaLtAnycompatibleCImp, MYS_CMP_LT)
DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleLeByteaCImp, byteaLeAnycompatibleCImp, MYS_CMP_LE)
DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleGtByteaCImp, byteaGtAnycompatibleCImp, MYS_CMP_GT)
DEFINE_ANY_BYTEA_CMP_PAIR(anycompatibleGeByteaCImp, byteaGeAnycompatibleCImp, MYS_CMP_GE)

PG_FUNCTION_INFO_V1(byteaGtByteaCImp);
Datum
byteaGtByteaCImp(PG_FUNCTION_ARGS)
{
	return byteaCmpBytea(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), MYS_CMP_GT);
}

PG_FUNCTION_INFO_V1(byteaGeByteaCImp);
Datum
byteaGeByteaCImp(PG_FUNCTION_ARGS)
{
	return byteaCmpBytea(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), MYS_CMP_GE);
}

PG_FUNCTION_INFO_V1(byteaLtByteaCImp);
Datum
byteaLtByteaCImp(PG_FUNCTION_ARGS)
{
	return byteaCmpBytea(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), MYS_CMP_LT);
}

PG_FUNCTION_INFO_V1(byteaLeByteaCImp);
Datum
byteaLeByteaCImp(PG_FUNCTION_ARGS)
{
	return byteaCmpBytea(PG_GETARG_DATUM(0), PG_GETARG_DATUM(1), MYS_CMP_LE);
}

/* ----------------------------------------------------------------
 *	3-valued logical AND (&&) and OR (||) operators
 * ----------------------------------------------------------------
 */

static bool
datum_is_truthy(Oid typid, Datum val)
{
	Datum		num = convertParameterToNumeric(typid, val);
	Datum		zero = DirectFunctionCall1(int4_numeric, Int32GetDatum(0));

	return DatumGetBool(DirectFunctionCall2(numeric_ne, num, zero));
}

static Datum
eval_three_valued_and(FunctionCallInfo fcinfo,
					  bool null0, Oid type0, Datum val0,
					  bool null1, Oid type1, Datum val1)
{
	bool		b0 = false;
	bool		b1 = false;

	if (!null0)
	{
		b0 = datum_is_truthy(type0, val0);
		if (!b0)
			PG_RETURN_BOOL(false);
	}
	if (!null1)
	{
		b1 = datum_is_truthy(type1, val1);
		if (!b1)
			PG_RETURN_BOOL(false);
	}
	if (null0 || null1)
		PG_RETURN_NULL();

	PG_RETURN_BOOL(true);
}

static Datum
eval_three_valued_or(FunctionCallInfo fcinfo,
					 bool null0, Oid type0, Datum val0,
					 bool null1, Oid type1, Datum val1)
{
	if (!null0 && datum_is_truthy(type0, val0))
		PG_RETURN_BOOL(true);
	if (!null1 && datum_is_truthy(type1, val1))
		PG_RETURN_BOOL(true);
	if (null0 || null1)
		PG_RETURN_NULL();

	PG_RETURN_BOOL(false);
}

PG_FUNCTION_INFO_V1(anycompatibleCondAndByteaCImp);
Datum
anycompatibleCondAndByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return eval_three_valued_and(fcinfo,
								 PG_ARGISNULL(0), t0,
								 PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								 PG_ARGISNULL(1), BYTEAOID,
								 PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

PG_FUNCTION_INFO_V1(byteaCondAndAnycompatibleCImp);
Datum
byteaCondAndAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return eval_three_valued_and(fcinfo,
								 PG_ARGISNULL(0), BYTEAOID,
								 PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								 PG_ARGISNULL(1), t1,
								 PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

PG_FUNCTION_INFO_V1(byteaCondAndByteaCImp);
Datum
byteaCondAndByteaCImp(PG_FUNCTION_ARGS)
{
	return eval_three_valued_and(fcinfo,
								 PG_ARGISNULL(0), BYTEAOID,
								 PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								 PG_ARGISNULL(1), BYTEAOID,
								 PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

PG_FUNCTION_INFO_V1(anycompatibleCondOrByteaCImp);
Datum
anycompatibleCondOrByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return eval_three_valued_or(fcinfo,
								PG_ARGISNULL(0), t0,
								PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								PG_ARGISNULL(1), BYTEAOID,
								PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

PG_FUNCTION_INFO_V1(byteaCondOrAnycompatibleCImp);
Datum
byteaCondOrAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return eval_three_valued_or(fcinfo,
								PG_ARGISNULL(0), BYTEAOID,
								PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								PG_ARGISNULL(1), t1,
								PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

PG_FUNCTION_INFO_V1(byteaCondOrByteaCImp);
Datum
byteaCondOrByteaCImp(PG_FUNCTION_ARGS)
{
	return eval_three_valued_or(fcinfo,
								PG_ARGISNULL(0), BYTEAOID,
								PG_ARGISNULL(0) ? (Datum) 0 : PG_GETARG_DATUM(0),
								PG_ARGISNULL(1), BYTEAOID,
								PG_ARGISNULL(1) ? (Datum) 0 : PG_GETARG_DATUM(1));
}

/* ----------------------------------------------------------------
 *	LIKE / NOT LIKE and REGEXP / RLIKE operators
 * ----------------------------------------------------------------
 */

static Datum
bytea_to_valid_text_datum(Datum d)
{
	return DirectFunctionCall1(mys_bytea2text, d);
}

static Oid
resolve_like_collation(Oid call_collid, PGFunction *like_fn, bool negate)
{
	if (OidIsValid(call_collid) && call_collid != DEFAULT_COLLATION_OID)
	{
		*like_fn = negate ? textnlike : textlike;
		return call_collid;
	}
	if (OidIsValid(getCaseInsensitiveId()))
	{
		*like_fn = negate ? textnlike : textlike;
		return getCaseInsensitiveId();
	}
	*like_fn = negate ? texticnlike : texticlike;
	return DEFAULT_COLLATION_OID;
}

PG_FUNCTION_INFO_V1(anycompatibleLikeByteaCImp);
Datum
anycompatibleLikeByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			parm0Type = getParameterType(fcinfo->flinfo, 0);
	Datum		s = convertParameterToText(parm0Type, PG_GETARG_DATUM(0));
	Datum		p = bytea_to_valid_text_datum(PG_GETARG_DATUM(1));
	PGFunction	fn;
	Oid			coll = resolve_like_collation(PG_GET_COLLATION(), &fn, false);

	return DirectFunctionCall2Coll(fn, coll, s, p);
}

PG_FUNCTION_INFO_V1(byteaLikeAnycompatibleCImp);
Datum
byteaLikeAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			parm1Type = getParameterType(fcinfo->flinfo, 1);
	Datum		s = bytea_to_valid_text_datum(PG_GETARG_DATUM(0));
	Datum		p = convertParameterToText(parm1Type, PG_GETARG_DATUM(1));
	PGFunction	fn;
	Oid			coll = resolve_like_collation(PG_GET_COLLATION(), &fn, false);

	return DirectFunctionCall2Coll(fn, coll, s, p);
}

PG_FUNCTION_INFO_V1(anycompatibleNlikeByteaCImp);
Datum
anycompatibleNlikeByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			parm0Type = getParameterType(fcinfo->flinfo, 0);
	Datum		s = convertParameterToText(parm0Type, PG_GETARG_DATUM(0));
	Datum		p = bytea_to_valid_text_datum(PG_GETARG_DATUM(1));
	PGFunction	fn;
	Oid			coll = resolve_like_collation(PG_GET_COLLATION(), &fn, true);

	return DirectFunctionCall2Coll(fn, coll, s, p);
}

PG_FUNCTION_INFO_V1(byteaNlikeAnycompatibleCImp);
Datum
byteaNlikeAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			parm1Type = getParameterType(fcinfo->flinfo, 1);
	Datum		s = bytea_to_valid_text_datum(PG_GETARG_DATUM(0));
	Datum		p = convertParameterToText(parm1Type, PG_GETARG_DATUM(1));
	PGFunction	fn;
	Oid			coll = resolve_like_collation(PG_GET_COLLATION(), &fn, true);

	return DirectFunctionCall2Coll(fn, coll, s, p);
}

/*
 * PostgreSQL's regex engine rejects nondeterministic ICU collations, so we
 * always pass DEFAULT_COLLATION_OID for case-insensitive REGEXP and
 * C_COLLATION_OID for binary REGEXP, after validating UTF-8 on bytea inputs.
 */
PG_FUNCTION_INFO_V1(anycompatibleRegexpByteaCImp);
Datum
anycompatibleRegexpByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2Coll(texticregexeq, DEFAULT_COLLATION_OID,
								   convertParameterToText(t0, PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaRegexpAnycompatibleCImp);
Datum
byteaRegexpAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2Coll(texticregexeq, DEFAULT_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   convertParameterToText(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleNregexpByteaCImp);
Datum
anycompatibleNregexpByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2Coll(texticregexne, DEFAULT_COLLATION_OID,
								   convertParameterToText(t0, PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaNregexpAnycompatibleCImp);
Datum
byteaNregexpAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2Coll(texticregexne, DEFAULT_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   convertParameterToText(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleRegexpbinByteaCImp);
Datum
anycompatibleRegexpbinByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2Coll(textregexeq, C_COLLATION_OID,
								   convertParameterToText(t0, PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaRegexpbinAnycompatibleCImp);
Datum
byteaRegexpbinAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2Coll(textregexeq, C_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   convertParameterToText(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleNregexpbinByteaCImp);
Datum
anycompatibleNregexpbinByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2Coll(textregexne, C_COLLATION_OID,
								   convertParameterToText(t0, PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaNregexpbinAnycompatibleCImp);
Datum
byteaNregexpbinAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2Coll(textregexne, C_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   convertParameterToText(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaRegexpByteaCImp);
Datum
byteaRegexpByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2Coll(texticregexeq, DEFAULT_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaNregexpByteaCImp);
Datum
byteaNregexpByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2Coll(texticregexne, DEFAULT_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaRegexpbinByteaCImp);
Datum
byteaRegexpbinByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2Coll(textregexeq, C_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaNregexpbinByteaCImp);
Datum
byteaNregexpbinByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2Coll(textregexne, C_COLLATION_OID,
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(0)),
								   bytea_to_valid_text_datum(PG_GETARG_DATUM(1)));
}

/* ----------------------------------------------------------------
 *	Arithmetic (+, -, *, /, %) and bitwise (&, |) operators
 * ----------------------------------------------------------------
 */

static Datum
numeric_div_mysql_null(Datum p0, Datum p1)
{
	Datum		zero = DirectFunctionCall1(int4_numeric, Int32GetDatum(0));

	if (DatumGetBool(DirectFunctionCall2(numeric_eq, p1, zero)))
		return (Datum) 0;
	return DirectFunctionCall2(numeric_div, p0, p1);
}

static Datum
numeric_mod_mysql_null(Datum p0, Datum p1)
{
	Datum		zero = DirectFunctionCall1(int4_numeric, Int32GetDatum(0));

	if (DatumGetBool(DirectFunctionCall2(numeric_eq, p1, zero)))
		return (Datum) 0;
	return DirectFunctionCall2(numeric_mod, p0, p1);
}

PG_FUNCTION_INFO_V1(byteaPlByteaCImp);
Datum
byteaPlByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(numeric_add,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatiblePlByteaCImp);
Datum
anycompatiblePlByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2(numeric_add,
							   convertParameterToNumeric(t0, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaPlAnycompatibleCImp);
Datum
byteaPlAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2(numeric_add,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaMiByteaCImp);
Datum
byteaMiByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(numeric_sub,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleMiByteaCImp);
Datum
anycompatibleMiByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2(numeric_sub,
							   convertParameterToNumeric(t0, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaMiAnycompatibleCImp);
Datum
byteaMiAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2(numeric_sub,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaMulByteaCImp);
Datum
byteaMulByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(numeric_mul,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleMulByteaCImp);
Datum
anycompatibleMulByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2(numeric_mul,
							   convertParameterToNumeric(t0, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaMulAnycompatibleCImp);
Datum
byteaMulAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2(numeric_mul,
							   convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToNumeric(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaDivByteaCImp);
Datum
byteaDivByteaCImp(PG_FUNCTION_ARGS)
{
	Datum		res = numeric_div_mysql_null(convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(anycompatibleDivByteaCImp);
Datum
anycompatibleDivByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);
	Datum		res = numeric_div_mysql_null(convertParameterToNumeric(t0, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(byteaDivAnycompatibleCImp);
Datum
byteaDivAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);
	Datum		res = numeric_div_mysql_null(convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(t1, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(byteaModByteaCImp);
Datum
byteaModByteaCImp(PG_FUNCTION_ARGS)
{
	Datum		res = numeric_mod_mysql_null(convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(anycompatibleModByteaCImp);
Datum
anycompatibleModByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);
	Datum		res = numeric_mod_mysql_null(convertParameterToNumeric(t0, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(byteaModAnycompatibleCImp);
Datum
byteaModAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);
	Datum		res = numeric_mod_mysql_null(convertParameterToNumeric(BYTEAOID, PG_GETARG_DATUM(0)),
											 convertParameterToNumeric(t1, PG_GETARG_DATUM(1)));

	if (res == (Datum) 0)
		PG_RETURN_NULL();
	return res;
}

PG_FUNCTION_INFO_V1(byteaAndByteaCImp);
Datum
byteaAndByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(int8and,
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleAndByteaCImp);
Datum
anycompatibleAndByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2(int8and,
							   convertParameterToInt8(t0, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaAndAnycompatibleCImp);
Datum
byteaAndAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2(int8and,
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(t1, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaOrByteaCImp);
Datum
byteaOrByteaCImp(PG_FUNCTION_ARGS)
{
	return DirectFunctionCall2(int8or,
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(anycompatibleOrByteaCImp);
Datum
anycompatibleOrByteaCImp(PG_FUNCTION_ARGS)
{
	Oid			t0 = getParameterType(fcinfo->flinfo, 0);

	return DirectFunctionCall2(int8or,
							   convertParameterToInt8(t0, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(1)));
}

PG_FUNCTION_INFO_V1(byteaOrAnycompatibleCImp);
Datum
byteaOrAnycompatibleCImp(PG_FUNCTION_ARGS)
{
	Oid			t1 = getParameterType(fcinfo->flinfo, 1);

	return DirectFunctionCall2(int8or,
							   convertParameterToInt8(BYTEAOID, PG_GETARG_DATUM(0)),
							   convertParameterToInt8(t1, PG_GETARG_DATUM(1)));
}

/* ----------------------------------------------------------------
 *	Internal parameter conversion helpers
 * ----------------------------------------------------------------
 */

static Oid
getParameterType(FmgrInfo *fmgrinfo, int parmIndex)
{
	Oid			parmType = get_fn_expr_argtype(fmgrinfo, parmIndex);

	if (!OidIsValid(parmType))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("could not determine data type of input")));

	return parmType;
}

/*
 * Parse the leading numeric prefix of a MySQL string / user variable into a
 * clean ASCII numeric string (returns "0" if no valid leading number).
 */
static void
extract_mysql_leading_number(const char *src, size_t srcLen, char **out_str)
{
	size_t		i = 0;
	size_t		start;
	size_t		numStart;
	bool		sawDigits = false;
	size_t		endPos;

	while (i < srcLen && isspace((unsigned char) src[i]))
		i++;

	start = i;
	if (i < srcLen && (src[i] == '+' || src[i] == '-'))
		i++;

	numStart = i;
	while (i < srcLen && isdigit((unsigned char) src[i]))
	{
		sawDigits = true;
		i++;
	}

	if (i < srcLen && src[i] == '.')
	{
		i++;
		while (i < srcLen && isdigit((unsigned char) src[i]))
		{
			sawDigits = true;
			i++;
		}
	}

	if (!sawDigits)
	{
		*out_str = pstrdup("0");
		return;
	}

	endPos = i;
	if (i < srcLen && (src[i] == 'e' || src[i] == 'E'))
	{
		size_t		expIdx = i + 1;

		if (expIdx < srcLen && (src[expIdx] == '+' || src[expIdx] == '-'))
			expIdx++;
		if (expIdx < srcLen && isdigit((unsigned char) src[expIdx]))
		{
			while (expIdx < srcLen && isdigit((unsigned char) src[expIdx]))
				expIdx++;
			endPos = expIdx;
		}
	}

	(void) numStart;
	*out_str = pnstrdup(src + start, endPos - start);
}

static void
truncateStrTailWhiteSpacesForDigit(size_t strLen, char *str)
{
	bool		meetNumber = false;
	bool		meetWhiteSpace = false;
	size_t		index;

	for (index = 0; index < strLen; index++)
	{
		unsigned char ch = (unsigned char) str[index];

		if (meetNumber)
		{
			if (!meetWhiteSpace)
			{
				if (ch == ' ' || ch == '\t' || ch == '\n')
				{
					meetWhiteSpace = true;
					str[index] = '\0';
				}
			}
			else
			{
				if (ch != ' ' && ch != '\t' && ch != '\n')
				{
					/* Non-whitespace after trailing space: restore space so PG rejects it */
					str[index - 1] = ' ';
					return;
				}
				str[index] = '\0';
			}
		}
		else if (isdigit(ch) || ch == '.')
		{
			meetNumber = true;
		}
	}
}

static Datum
convertParameterToText(Oid parmType, Datum parmVal)
{
	if (parmType == TEXTOID || parmType == VARCHAROID)
		return parmVal;
	if (parmType == BPCHAROID)
		return DirectFunctionCall1(rtrim1, parmVal);
	if (parmType == BOOLOID)
	{
		const char *s = DatumGetBool(parmVal) ? "1" : "0";

		return PointerGetDatum(cstring_to_text_with_len(s, 1));
	}
	else
	{
		Oid			typOutput;
		bool		typIsVarlena;
		char	   *str;
		text	   *res;

		getTypeOutputInfo(parmType, &typOutput, &typIsVarlena);
		str = OidOutputFunctionCall(typOutput, parmVal);
		res = cstring_to_text(str);
		pfree(str);
		return PointerGetDatum(res);
	}
}

static int64
varbit_to_int64_checked(VarBit *vb)
{
	int32		bitLen = VARBITLEN(vb);
	uint8	   *sp = VARBITS(vb);
	uint64		acc = 0;
	int32		k;

	if (bitLen > 64)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	for (k = 0; k < bitLen; k++)
	{
		acc <<= 1;
		if (sp[k / BITS_PER_BYTE] & (0x80 >> (k % BITS_PER_BYTE)))
			acc |= 1;
	}

	if (acc > (uint64) PG_INT64_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bigint out of range")));

	return (int64) acc;
}

static Datum
cstring_leading_to_numeric(const char *s, size_t len)
{
	char	   *numStr = NULL;
	Datum		res;

	extract_mysql_leading_number(s, len, &numStr);
	res = DirectFunctionCall3(numeric_in,
							  CStringGetDatum(numStr),
							  ObjectIdGetDatum(InvalidOid),
							  Int32GetDatum(-1));
	pfree(numStr);
	return res;
}

static Datum
convertParameterToNumeric(Oid parmType, Datum parm)
{
	char		parmStr[MAXDATELEN + 1];

	switch (parmType)
	{
		case INT2OID:
			return DirectFunctionCall1(int2_numeric, parm);
		case INT4OID:
			return DirectFunctionCall1(int4_numeric, parm);
		case INT8OID:
			return DirectFunctionCall1(int8_numeric, parm);
		case FLOAT4OID:
			return DirectFunctionCall1(float4_numeric, parm);
		case FLOAT8OID:
			return DirectFunctionCall1(float8_numeric, parm);
		case NUMERICOID:
			return parm;
		case TEXTOID:
		case VARCHAROID:
		case BPCHAROID:
			{
				text	   *t = DatumGetTextPP(parm);

				return cstring_leading_to_numeric(VARDATA_ANY(t), VARSIZE_ANY_EXHDR(t));
			}
		case DATEOID:
			format_date_numeric_str(DatumGetDateADT(parm), parmStr);
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum(parmStr),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
		case TIMEOID:
			format_time_numeric_str(DatumGetTimeADT(parm), parmStr, sizeof(parmStr));
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum(parmStr),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
		case TIMETZOID:
			{
				/* Copy TimeTzADT locally; never mutate pass-by-reference Datum! */
				TimeTzADT	local_tz = *DatumGetTimeTzADTP(parm);

				format_time_numeric_str(local_tz.time, parmStr, sizeof(parmStr));
				return DirectFunctionCall3(numeric_in,
										   CStringGetDatum(parmStr),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1));
			}
		case TIMESTAMPOID:
			format_timestamp_numeric_str(DatumGetTimestamp(parm), NULL, parmStr);
			return DirectFunctionCall3(numeric_in,
									   CStringGetDatum(parmStr),
									   ObjectIdGetDatum(InvalidOid),
									   Int32GetDatum(-1));
		case TIMESTAMPTZOID:
			{
				int			tz = 0;

				format_timestamp_numeric_str(DatumGetTimestampTz(parm), &tz, parmStr);
				return DirectFunctionCall3(numeric_in,
										   CStringGetDatum(parmStr),
										   ObjectIdGetDatum(InvalidOid),
										   Int32GetDatum(-1));
			}
		case BOOLOID:
			return DirectFunctionCall1(int4_numeric,
									   Int32GetDatum(DatumGetBool(parm) ? 1 : 0));
		case BITOID:
		case VARBITOID:
			return DirectFunctionCall1(int8_numeric,
									   Int64GetDatum(varbit_to_int64_checked(DatumGetVarBitP(parm))));
		case BYTEAOID:
			return bytea2numeric_internal(DatumGetByteaPP(parm));
		default:
			if (mys_binary_base_type(parmType) == BYTEAOID)
				return bytea2numeric_internal(DatumGetByteaPP(parm));
			{
				Oid			typOutput;
				bool		typIsVarlena;
				char	   *str;
				Datum		res;

				getTypeOutputInfo(parmType, &typOutput, &typIsVarlena);
				str = OidOutputFunctionCall(typOutput, parm);
				res = cstring_leading_to_numeric(str, strlen(str));
				pfree(str);
				return res;
			}
	}
}

static Datum
numeric_datum_to_int8_rounded(Datum numDatum)
{
	Datum		rounded = DirectFunctionCall2(numeric_round, numDatum, Int32GetDatum(0));

	return DirectFunctionCall1(numeric_int8, rounded);
}

static Datum
convertParameterToInt8(Oid parmType, Datum parm)
{
	switch (parmType)
	{
		case INT2OID:
			return DirectFunctionCall1(int28, parm);
		case INT4OID:
			return DirectFunctionCall1(int48, parm);
		case INT8OID:
			return parm;
		case FLOAT4OID:
			return DirectFunctionCall1(ftoi8, parm);
		case FLOAT8OID:
			return DirectFunctionCall1(dtoi8, parm);
		case NUMERICOID:
			return numeric_datum_to_int8_rounded(parm);
		case BOOLOID:
			return Int64GetDatum(DatumGetBool(parm) ? 1 : 0);
		case BITOID:
		case VARBITOID:
			return Int64GetDatum(varbit_to_int64_checked(DatumGetVarBitP(parm)));
		case BYTEAOID:
			return bytea2int8_internal(DatumGetByteaPP(parm));
		default:
			if (mys_binary_base_type(parmType) == BYTEAOID)
				return bytea2int8_internal(DatumGetByteaPP(parm));
			return numeric_datum_to_int8_rounded(convertParameterToNumeric(parmType, parm));
	}
}

static Datum
bytea2int8_internal(bytea *vlena)
{
	return numeric_datum_to_int8_rounded(bytea2numeric_internal(vlena));
}

static Datum
bytea2numeric_internal(bytea *vlena)
{
	return cstring_leading_to_numeric(VARDATA_ANY(vlena), VARSIZE_ANY_EXHDR(vlena));
}

static char *
AppendSeconds(char *cp, int sec, fsec_t fsec, int precision, bool fillzeros)
{
	Assert(precision >= 0);

	if (fillzeros)
		cp = pg_ultostr_zeropad(cp, sec, 2);
	else
		cp = pg_ultostr(cp, sec);

	if (fsec != 0)
	{
		int32		value = Abs(fsec);
		char	   *end = &cp[precision + 1];
		bool		gotnonzero = false;

		*cp++ = '.';

		while (precision--)
		{
			int32		oldval = value;
			int32		remainder;

			value /= 10;
			remainder = oldval - value * 10;

			if (remainder)
				gotnonzero = true;

			if (gotnonzero)
				cp[precision] = '0' + remainder;
			else
				end = &cp[precision];
		}

		return end;
	}
	else
		return cp;
}
