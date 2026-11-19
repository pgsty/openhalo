/*-------------------------------------------------------------------------
 *
 * time_func.c
 *	  MySQL SUBTIME(), ADDTIME(), and TIMEDIFF() functions
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "common/int.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/mysql/mys_date.h"
#include "utils/timestamp.h"

static text *
mys_add_or_sub_time(text *text0, text *text1, bool is_sub)
{
	char	   *str0 = text_to_cstring(text0);
	char	   *str1 = text_to_cstring(text1);
	struct pg_tm tm0;
	struct pg_tm tm1;
	fsec_t		fsec0 = 0;
	fsec_t		fsec1 = 0;
	int			sign0 = 1;
	int			sign1 = 1;
	int			eff_sign1;

	memset(&tm0, 0, sizeof(tm0));
	memset(&tm1, 0, sizeof(tm1));

	mys_pre_time_in_for_subtime(str0, &tm0, &fsec0, &sign0);
	mys_pre_time_in_for_subtime(str1, &tm1, &fsec1, &sign1);

	pfree(str0);
	pfree(str1);

	if (tm1.tm_year > 0)
		return NULL;

	eff_sign1 = is_sub ? -sign1 : sign1;

	if (tm0.tm_year > 0)
	{
		Timestamp	ts0;
		TimeADT		t1;
		Timestamp	tempResult;
		int64		delta;
		char	   *out;
		text	   *res;

		if (sign0 < 0 || tm2timestamp(&tm0, fsec0, NULL, &ts0) != 0)
			return NULL;
		if (tm2time(&tm1, fsec1, &t1) != 0)
			return NULL;

		delta = (eff_sign1 < 0) ? -t1 : t1;
		if (pg_add_s64_overflow(ts0, delta, &tempResult) ||
			!IS_VALID_TIMESTAMP(tempResult))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		out = DatumGetCString(DirectFunctionCall1(timestamp_out,
												  TimestampGetDatum(tempResult)));
		res = cstring_to_text(out);
		pfree(out);
		return res;
	}
	else
	{
		TimeADT		t0;
		TimeADT		t1;
		TimeADT		s0;
		TimeADT		s1;
		TimeADT		tempResult;
		struct pg_tm tempTm;
		fsec_t		tempFsec;
		char	   *resultStr;
		text	   *res;

		if (tm2time(&tm0, fsec0, &t0) != 0 || tm2time(&tm1, fsec1, &t1) != 0)
			return NULL;

		s0 = (sign0 < 0) ? -t0 : t0;
		s1 = (eff_sign1 < 0) ? -t1 : t1;
		if (pg_add_s64_overflow(s0, s1, &tempResult))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));

		resultStr = DatumGetCString(DirectFunctionCall1(time_out,
														TimeADTGetDatum(tempResult)));
		time2tm(tempResult, &tempTm, &tempFsec);
		if (tempTm.tm_hour > MYS_MAX_TIME_HOUR || tempTm.tm_hour < -MYS_MAX_TIME_HOUR)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("Truncated incorrect time value: '%s'", resultStr)));

		res = cstring_to_text(resultStr);
		pfree(resultStr);
		return res;
	}
}

PG_FUNCTION_INFO_V1(subtime);
Datum
subtime(PG_FUNCTION_ARGS)
{
	text	   *text0 = PG_GETARG_TEXT_PP(0);
	text	   *text1 = PG_GETARG_TEXT_PP(1);
	text	   *result = mys_add_or_sub_time(text0, text1, true);

	PG_FREE_IF_COPY(text0, 0);
	PG_FREE_IF_COPY(text1, 1);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(addtime);
Datum
addtime(PG_FUNCTION_ARGS)
{
	text	   *text0 = PG_GETARG_TEXT_PP(0);
	text	   *text1 = PG_GETARG_TEXT_PP(1);
	text	   *result = mys_add_or_sub_time(text0, text1, false);

	PG_FREE_IF_COPY(text0, 0);
	PG_FREE_IF_COPY(text1, 1);

	if (result == NULL)
		PG_RETURN_NULL();
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(timediff);
Datum
timediff(PG_FUNCTION_ARGS)
{
	text	   *text0 = PG_GETARG_TEXT_PP(0);
	text	   *text1 = PG_GETARG_TEXT_PP(1);
	char	   *str0 = text_to_cstring(text0);
	char	   *str1 = text_to_cstring(text1);
	struct pg_tm tm0;
	struct pg_tm tm1;
	struct pg_tm tempTm;
	fsec_t		fsec0 = 0;
	fsec_t		fsec1 = 0;
	fsec_t		tempFsec;
	int			sign0 = 1;
	int			sign1 = 1;
	TimeADT		result = 0;
	char	   *resultStr;

	memset(&tm0, 0, sizeof(tm0));
	memset(&tm1, 0, sizeof(tm1));

	mys_pre_time_in_for_subtime(str0, &tm0, &fsec0, &sign0);
	mys_pre_time_in_for_subtime(str1, &tm1, &fsec1, &sign1);

	pfree(str0);
	pfree(str1);
	PG_FREE_IF_COPY(text0, 0);
	PG_FREE_IF_COPY(text1, 1);

	if ((tm0.tm_year > 0) != (tm1.tm_year > 0))
		PG_RETURN_NULL();

	if (tm0.tm_year > 0)
	{
		Timestamp	ts0;
		Timestamp	ts1;

		if (sign0 < 0 || sign1 < 0 ||
			tm2timestamp(&tm0, fsec0, NULL, &ts0) != 0 ||
			tm2timestamp(&tm1, fsec1, NULL, &ts1) != 0)
			PG_RETURN_NULL();

		if (pg_sub_s64_overflow(ts0, ts1, &result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));
	}
	else
	{
		TimeADT		t0;
		TimeADT		t1;
		TimeADT		s0;
		TimeADT		s1;

		if (tm2time(&tm0, fsec0, &t0) != 0 || tm2time(&tm1, fsec1, &t1) != 0)
			PG_RETURN_NULL();

		s0 = (sign0 < 0) ? -t0 : t0;
		s1 = (sign1 < 0) ? -t1 : t1;
		if (pg_sub_s64_overflow(s0, s1, &result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));
	}

	resultStr = DatumGetCString(DirectFunctionCall1(time_out,
													TimeADTGetDatum(result)));
	time2tm(result, &tempTm, &tempFsec);
	if (tempTm.tm_hour > MYS_MAX_TIME_HOUR || tempTm.tm_hour < -MYS_MAX_TIME_HOUR)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("Truncated incorrect time value: '%s'", resultStr)));

	pfree(resultStr);
	PG_RETURN_TIMEADT(result);
}
