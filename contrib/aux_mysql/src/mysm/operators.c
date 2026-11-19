/*-------------------------------------------------------------------------
 *
 * operators.c
 *	  MySQL-compatible boolean and timestamp/text operators
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "adapter/mysql/systemVar.h"
#include "datatype/timestamp.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/fmgrprotos.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"
#include "utils/timestamp.h"

PG_FUNCTION_INFO_V1(boolCondAndboolCImp);
Datum
boolCondAndboolCImp(PG_FUNCTION_ARGS)
{
	bool		null1 = PG_ARGISNULL(0);
	bool		null2 = PG_ARGISNULL(1);
	bool		arg1 = null1 ? false : PG_GETARG_BOOL(0);
	bool		arg2 = null2 ? false : PG_GETARG_BOOL(1);

	if (null1 && null2)
		PG_RETURN_NULL();
	if (null1)
	{
		if (!arg2)
			PG_RETURN_BOOL(false);
		PG_RETURN_NULL();
	}
	if (null2)
	{
		if (!arg1)
			PG_RETURN_BOOL(false);
		PG_RETURN_NULL();
	}
	PG_RETURN_BOOL(arg1 && arg2);
}

PG_FUNCTION_INFO_V1(boolOrBool);
Datum
boolOrBool(PG_FUNCTION_ARGS)
{
	bool		null1 = PG_ARGISNULL(0);
	bool		null2 = PG_ARGISNULL(1);
	bool		arg1 = null1 ? false : PG_GETARG_BOOL(0);
	bool		arg2 = null2 ? false : PG_GETARG_BOOL(1);

	if (!null1 && !null2)
		PG_RETURN_BOOL(arg1 || arg2);
	if (!null1)
	{
		if (arg1)
			PG_RETURN_BOOL(true);
		PG_RETURN_NULL();
	}
	if (!null2)
	{
		if (arg2)
			PG_RETURN_BOOL(true);
		PG_RETURN_NULL();
	}
	PG_RETURN_NULL();
}

static int
mys_cmp_int(int a, int b)
{
	return (a < b) ? -1 : ((a > b) ? 1 : 0);
}

static int
mys_timestampCmpText(Timestamp arg1, text *arg2)
{
	char	   *argString2;
	int			timeFlags = MYS_TIME_FUZZY_DATE | MYS_TIME_INVALID_DATES;
	struct pg_tm tm1;
	struct pg_tm tm2;
	fsec_t		fsec1 = 0;
	fsec_t		fsec2 = 0;
	int			cmp;

	if (TIMESTAMP_IS_NOBEGIN(arg1))
		return -1;
	if (TIMESTAMP_IS_NOEND(arg1))
		return 1;

	if (timestamp2tm(arg1, NULL, &tm1, &fsec1, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	if (mys_sqlMode & MYS_MODE_NO_ZERO_IN_DATE)
		timeFlags |= MYS_TIME_NO_ZERO_IN_DATE;
	if (mys_sqlMode & MYS_MODE_NO_ZERO_DATE)
		timeFlags |= MYS_TIME_NO_ZERO_DATE;

	argString2 = text_to_cstring(arg2);
	memset(&tm2, 0, sizeof(tm2));

	if (mys_DecodeStringDatetimeForCompare(argString2, &tm2, &fsec2, timeFlags))
	{
		pfree(argString2);
		if ((cmp = mys_cmp_int(tm1.tm_year, tm2.tm_year)) != 0)
			return cmp;
		if ((cmp = mys_cmp_int(tm1.tm_mon, tm2.tm_mon)) != 0)
			return cmp;
		if ((cmp = mys_cmp_int(tm1.tm_mday, tm2.tm_mday)) != 0)
			return cmp;
		if ((cmp = mys_cmp_int(tm1.tm_hour, tm2.tm_hour)) != 0)
			return cmp;
		if ((cmp = mys_cmp_int(tm1.tm_min, tm2.tm_min)) != 0)
			return cmp;
		if ((cmp = mys_cmp_int(tm1.tm_sec, tm2.tm_sec)) != 0)
			return cmp;
		return mys_cmp_int((int) fsec1, (int) fsec2);
	}
	else
	{
		char		buf1[MAXDATELEN + 1];

		EncodeDateTime(&tm1, fsec1, false, 0, NULL, USE_ISO_DATES, buf1);
		cmp = strcmp(buf1, argString2);
		pfree(argString2);
		return cmp;
	}
}

PG_FUNCTION_INFO_V1(mys_timestampGtText);
Datum
mys_timestampGtText(PG_FUNCTION_ARGS)
{
	Timestamp	arg1 = PG_GETARG_TIMESTAMP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result = (mys_timestampCmpText(arg1, arg2) > 0);

	PG_FREE_IF_COPY(arg2, 1);
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(mys_timestampGeText);
Datum
mys_timestampGeText(PG_FUNCTION_ARGS)
{
	Timestamp	arg1 = PG_GETARG_TIMESTAMP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result = (mys_timestampCmpText(arg1, arg2) >= 0);

	PG_FREE_IF_COPY(arg2, 1);
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(mys_timestampLtText);
Datum
mys_timestampLtText(PG_FUNCTION_ARGS)
{
	Timestamp	arg1 = PG_GETARG_TIMESTAMP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result = (mys_timestampCmpText(arg1, arg2) < 0);

	PG_FREE_IF_COPY(arg2, 1);
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(mys_timestampLeText);
Datum
mys_timestampLeText(PG_FUNCTION_ARGS)
{
	Timestamp	arg1 = PG_GETARG_TIMESTAMP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result = (mys_timestampCmpText(arg1, arg2) <= 0);

	PG_FREE_IF_COPY(arg2, 1);
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(mys_timestampEqText);
Datum
mys_timestampEqText(PG_FUNCTION_ARGS)
{
	Timestamp	arg1 = PG_GETARG_TIMESTAMP(0);
	text	   *arg2 = PG_GETARG_TEXT_PP(1);
	bool		result = (mys_timestampCmpText(arg1, arg2) == 0);

	PG_FREE_IF_COPY(arg2, 1);
	PG_RETURN_BOOL(result);
}

PG_FUNCTION_INFO_V1(mys_textPlInterval);
Datum
mys_textPlInterval(PG_FUNCTION_ARGS)
{
	text	   *arg0 = PG_GETARG_TEXT_PP(0);
	char	   *leftArgString = text_to_cstring(arg0);
	int			timeFlags = MYS_TIME_NO_ZERO_DATE;
	Timestamp	leftArg;
	Timestamp	result;

	PG_FREE_IF_COPY(arg0, 0);

	if (mys_sqlMode & MYS_MODE_INVALID_DATES)
		timeFlags |= MYS_TIME_INVALID_DATES;

	if (!mys_strToDatetimeInternal(leftArgString, timeFlags, &leftArg))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("Incorrect datetime value: '%s'", leftArgString)));

	pfree(leftArgString);
	result = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
												   TimestampGetDatum(leftArg),
												   IntervalPGetDatum(PG_GETARG_INTERVAL_P(1))));
	PG_RETURN_TIMESTAMP(result);
}

PG_FUNCTION_INFO_V1(mys_intervalPlText);
Datum
mys_intervalPlText(PG_FUNCTION_ARGS)
{
	text	   *arg1 = PG_GETARG_TEXT_PP(1);
	char	   *rightArgString = text_to_cstring(arg1);
	int			timeFlags = MYS_TIME_NO_ZERO_DATE;
	Timestamp	rightArg;
	Timestamp	result;

	PG_FREE_IF_COPY(arg1, 1);

	if (mys_sqlMode & MYS_MODE_INVALID_DATES)
		timeFlags |= MYS_TIME_INVALID_DATES;

	if (!mys_strToDatetimeInternal(rightArgString, timeFlags, &rightArg))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("Incorrect datetime value: '%s'", rightArgString)));

	pfree(rightArgString);
	result = DatumGetTimestamp(DirectFunctionCall2(timestamp_pl_interval,
												   TimestampGetDatum(rightArg),
												   IntervalPGetDatum(PG_GETARG_INTERVAL_P(0))));
	PG_RETURN_TIMESTAMP(result);
}

PG_FUNCTION_INFO_V1(mys_textMiInterval);
Datum
mys_textMiInterval(PG_FUNCTION_ARGS)
{
	text	   *arg0 = PG_GETARG_TEXT_PP(0);
	char	   *leftArgString = text_to_cstring(arg0);
	int			timeFlags = MYS_TIME_NO_ZERO_DATE;
	Timestamp	leftArg;
	Timestamp	result;

	PG_FREE_IF_COPY(arg0, 0);

	if (mys_sqlMode & MYS_MODE_INVALID_DATES)
		timeFlags |= MYS_TIME_INVALID_DATES;

	if (!mys_strToDatetimeInternal(leftArgString, timeFlags, &leftArg))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("Incorrect datetime value: '%s'", leftArgString)));

	pfree(leftArgString);
	result = DatumGetTimestamp(DirectFunctionCall2(timestamp_mi_interval,
												   TimestampGetDatum(leftArg),
												   IntervalPGetDatum(PG_GETARG_INTERVAL_P(1))));
	PG_RETURN_TIMESTAMP(result);
}
