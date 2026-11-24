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
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/datetime.h"
#include "utils/mysql/mys_date.h"
#include "utils/timestamp.h"

static void
format_mysql_time_checked(int64 total_usec, char *buf, size_t bufsize)
{
	bool		neg = (total_usec < 0);
	uint64		abs_usec = neg ? (uint64) (-(total_usec + 1)) + 1 : (uint64) total_usec;
	uint64		total_sec = abs_usec / USECS_PER_SEC;
	uint32		fsec = (uint32) (abs_usec % USECS_PER_SEC);
	uint64		hours = total_sec / SECS_PER_HOUR;
	uint32		rem = (uint32) (total_sec % SECS_PER_HOUR);
	uint32		mins = rem / SECS_PER_MINUTE;
	uint32		secs = rem % SECS_PER_MINUTE;

	if (fsec == 0)
		pg_snprintf(buf, bufsize, "%s%02llu:%02u:%02u",
					neg ? "-" : "", (unsigned long long) hours, mins, secs);
	else
	{
		char		fbuf[16];
		int			flen = pg_snprintf(fbuf, sizeof(fbuf), ".%06u", fsec);

		while (flen > 0 && fbuf[flen - 1] == '0')
			fbuf[--flen] = '\0';
		pg_snprintf(buf, bufsize, "%s%02llu:%02u:%02u%s",
					neg ? "-" : "", (unsigned long long) hours, mins, secs, fbuf);
	}

	if (hours > MYS_MAX_TIME_HOUR)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("Truncated incorrect time value: '%s'", buf)));
}

/* The second argument is a signed duration, never a datetime. */
static bool
parse_duration(text *value, int64 *usecs)
{
	char	   *str = text_to_cstring(value);
	struct pg_tm tm = {0};
	fsec_t		fsec = 0;
	int			sign;

	mys_pre_time_in_for_subtime(str, &tm, &fsec, &sign);
	pfree(str);
	if (tm.tm_year > 0 || tm2time(&tm, fsec, usecs) != 0)
		return false;
	if (sign < 0)
		*usecs = -*usecs;
	return true;
}

static bool
timestamp_plus_duration(Timestamp ts, int64 delta, Timestamp *result)
{
	/* The four-digit AD years shared by PostgreSQL and MySQL DATETIME. */
	const Timestamp first = (int64) (date2j(1, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY;
	const Timestamp end = (int64) (date2j(10000, 1, 1) - POSTGRES_EPOCH_JDATE) * USECS_PER_DAY;

	if (ts < first || ts >= end || pg_add_s64_overflow(ts, delta, result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));
	return *result >= first && *result < end;
}

static text *
mys_add_or_sub_time(text *text0, text *text1, bool is_sub)
{
	char	   *str0 = text_to_cstring(text0);
	struct pg_tm tm0 = {0};
	fsec_t		fsec0 = 0;
	int			sign0;
	int64		delta;

	mys_pre_time_in_for_subtime(str0, &tm0, &fsec0, &sign0);
	pfree(str0);
	if (!parse_duration(text1, &delta))
		return NULL;
	if (is_sub)
		delta = -delta;

	if (tm0.tm_year > 0)
	{
		Timestamp	ts;
		char		buf[MAXDATELEN + 1];

		if (sign0 < 0 || tm2timestamp(&tm0, fsec0, NULL, &ts) != 0)
			return NULL;
		if (!timestamp_plus_duration(ts, delta, &ts))
			return NULL;
		if (timestamp2tm(ts, NULL, &tm0, &fsec0, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		EncodeDateTime(&tm0, fsec0, false, 0, NULL, USE_ISO_DATES, buf);
		return cstring_to_text(buf);
	}
	else
	{
		TimeADT		t0;
		int64		result;
		char		buf[64];

		if (tm2time(&tm0, fsec0, &t0) != 0)
			return NULL;
		if (pg_add_s64_overflow(sign0 < 0 ? -t0 : t0, delta, &result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));
		format_mysql_time_checked(result, buf, sizeof(buf));
		return cstring_to_text(buf);
	}
}

static Datum
shift_timestamp(FunctionCallInfo fcinfo, bool subtract, bool text_duration)
{
	int64		delta;
	Timestamp	result;

	if (text_duration)
	{
		text	   *value = PG_GETARG_TEXT_PP(1);
		bool		valid = parse_duration(value, &delta);

		PG_FREE_IF_COPY(value, 1);
		if (!valid)
			PG_RETURN_NULL();
	}
	else
		delta = PG_GETARG_TIMEADT(1);
	if (!timestamp_plus_duration(PG_GETARG_TIMESTAMP(0), subtract ? -delta : delta, &result))
		PG_RETURN_NULL();
	PG_RETURN_TIMESTAMP(result);
}

PG_FUNCTION_INFO_V1(mys_addtime_timestamp_text);
Datum
mys_addtime_timestamp_text(PG_FUNCTION_ARGS)
{
	return shift_timestamp(fcinfo, false, true);
}

PG_FUNCTION_INFO_V1(mys_addtime_timestamp_time);
Datum
mys_addtime_timestamp_time(PG_FUNCTION_ARGS)
{
	return shift_timestamp(fcinfo, false, false);
}

PG_FUNCTION_INFO_V1(mys_subtime_timestamp_text);
Datum
mys_subtime_timestamp_text(PG_FUNCTION_ARGS)
{
	return shift_timestamp(fcinfo, true, true);
}

PG_FUNCTION_INFO_V1(mys_subtime_timestamp_time);
Datum
mys_subtime_timestamp_time(PG_FUNCTION_ARGS)
{
	return shift_timestamp(fcinfo, true, false);
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

static bool
mys_timediff_usec(text *text0, text *text1, int64 *result_out)
{
	char	   *str0 = text_to_cstring(text0);
	char	   *str1 = text_to_cstring(text1);
	struct pg_tm tm0;
	struct pg_tm tm1;
	fsec_t		fsec0 = 0;
	fsec_t		fsec1 = 0;
	int			sign0 = 1;
	int			sign1 = 1;
	int64		result = 0;

	memset(&tm0, 0, sizeof(tm0));
	memset(&tm1, 0, sizeof(tm1));

	mys_pre_time_in_for_subtime(str0, &tm0, &fsec0, &sign0);
	mys_pre_time_in_for_subtime(str1, &tm1, &fsec1, &sign1);

	pfree(str0);
	pfree(str1);

	if ((tm0.tm_year > 0) != (tm1.tm_year > 0))
		return false;

	if (tm0.tm_year > 0)
	{
		Timestamp	ts0;
		Timestamp	ts1;

		if (sign0 < 0 || sign1 < 0 ||
			tm2timestamp(&tm0, fsec0, NULL, &ts0) != 0 ||
			tm2timestamp(&tm1, fsec1, NULL, &ts1) != 0)
			return false;

		if (pg_sub_s64_overflow(ts0, ts1, &result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));
	}
	else
	{
		TimeADT		t0;
		TimeADT		t1;
		int64		s0;
		int64		s1;

		if (tm2time(&tm0, fsec0, &t0) != 0 || tm2time(&tm1, fsec1, &t1) != 0)
			return false;

		s0 = (sign0 < 0) ? -t0 : t0;
		s1 = (sign1 < 0) ? -t1 : t1;
		if (pg_sub_s64_overflow(s0, s1, &result))
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
					 errmsg("time value out of range")));
	}

	*result_out = result;
	return true;
}

PG_FUNCTION_INFO_V1(mys_timediff_text);
Datum
mys_timediff_text(PG_FUNCTION_ARGS)
{
	text	   *text0 = PG_GETARG_TEXT_PP(0);
	text	   *text1 = PG_GETARG_TEXT_PP(1);
	int64		result = 0;
	bool		ok;
	char		resultStr[64];

	ok = mys_timediff_usec(text0, text1, &result);
	PG_FREE_IF_COPY(text0, 0);
	PG_FREE_IF_COPY(text1, 1);

	if (!ok)
		PG_RETURN_NULL();

	format_mysql_time_checked(result, resultStr, sizeof(resultStr));
	PG_RETURN_TEXT_P(cstring_to_text(resultStr));
}

PG_FUNCTION_INFO_V1(mys_timediff_timestamp);
Datum
mys_timediff_timestamp(PG_FUNCTION_ARGS)
{
	Timestamp	lhs = PG_GETARG_TIMESTAMP(0);
	Timestamp	rhs = PG_GETARG_TIMESTAMP(1);
	int64		result;
	char		buf[64];

	if (TIMESTAMP_NOT_FINITE(lhs) || TIMESTAMP_NOT_FINITE(rhs) ||
		pg_sub_s64_overflow(lhs, rhs, &result))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time value out of range")));

	format_mysql_time_checked(result, buf, sizeof(buf));
	PG_RETURN_TEXT_P(cstring_to_text(buf));
}

/*
 * Legacy 1.x C entry point retained for pre-upgrade catalog ABI compatibility
 * (mysql.timediff returning PG time). Rejects negative / >24h values instead
 * of emitting corrupt TimeADT strings.
 */
PG_FUNCTION_INFO_V1(timediff);
Datum
timediff(PG_FUNCTION_ARGS)
{
	text	   *text0 = PG_GETARG_TEXT_PP(0);
	text	   *text1 = PG_GETARG_TEXT_PP(1);
	int64		result = 0;
	bool		ok;
	char		resultStr[64];

	ok = mys_timediff_usec(text0, text1, &result);
	PG_FREE_IF_COPY(text0, 0);
	PG_FREE_IF_COPY(text1, 1);

	if (!ok)
		PG_RETURN_NULL();

	format_mysql_time_checked(result, resultStr, sizeof(resultStr));
	if (result < 0 || result > HOURS_PER_DAY * USECS_PER_HOUR)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_FIELD_OVERFLOW),
				 errmsg("time value out of range for PostgreSQL time: '%s'", resultStr)));

	PG_RETURN_TIMEADT((TimeADT) result);
}
