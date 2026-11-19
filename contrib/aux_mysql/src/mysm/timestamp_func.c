/*-------------------------------------------------------------------------
 *
 * timestamp_func.c
 *	  MySQL TIMESTAMPDIFF() and EXTRACT() timestamp functions
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "datatype/timestamp.h"
#include "fmgr.h"
#include "parser/scansup.h"
#include "utils/datetime.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"

static TimeOffset time2t(const int hour, const int min, const int sec, const fsec_t fsec);
static Datum timestamp_part_common(PG_FUNCTION_ARGS, bool retnumeric);
static float8 NonFiniteTimestampTzPart(int type, int unit, char *lowunits, bool isNegative, bool isTz);

PG_FUNCTION_INFO_V1(mys_extract_timestamp);
Datum
mys_extract_timestamp(PG_FUNCTION_ARGS)
{
	return timestamp_part_common(fcinfo, true);
}

PG_FUNCTION_INFO_V1(timestampdiff);
Datum
timestampdiff(PG_FUNCTION_ARGS)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	ts1 = PG_GETARG_TIMESTAMP(1);
	Timestamp	ts2 = PG_GETARG_TIMESTAMP(2);
	bool		is_negative = (ts1 > ts2);
	fsec_t		fsec1;
	fsec_t		fsec2;
	struct pg_tm tt1;
	struct pg_tm tt2;
	struct pg_tm *tm1 = &tt1;
	struct pg_tm *tm2 = &tt2;
	char	   *lowunits;
	int			type;
	int			val;
	int64		year_diff;
	int64		month_diff;
	uint64		diff_usec;
	uint64		uval = 0;
	bool		use_uval = false;
	int64		intresult = 0;

	if (TIMESTAMP_NOT_FINITE(ts1) || TIMESTAMP_NOT_FINITE(ts2))
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("cannot subtract infinite timestamps")));

	if (is_negative)
	{
		Timestamp	temp_ts = ts1;

		ts1 = ts2;
		ts2 = temp_ts;
	}

	if (timestamp2tm(ts1, NULL, tm1, &fsec1, NULL, NULL) != 0 ||
		timestamp2tm(ts2, NULL, tm2, &fsec2, NULL, NULL) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
				 errmsg("timestamp out of range")));

	month_diff = (int64) tm2->tm_mon - (int64) tm1->tm_mon;
	if (tm2->tm_mday < tm1->tm_mday)
		month_diff -= 1;
	else if (tm2->tm_mday == tm1->tm_mday)
	{
		TimeOffset	time1 = time2t(tm1->tm_hour, tm1->tm_min, tm1->tm_sec, fsec1);
		TimeOffset	time2 = time2t(tm2->tm_hour, tm2->tm_min, tm2->tm_sec, fsec2);

		if (time2 < time1)
			month_diff -= 1;
	}

	year_diff = (int64) tm2->tm_year - (int64) tm1->tm_year;
	if (month_diff < 0)
	{
		month_diff = year_diff * 12 + month_diff;
		year_diff -= 1;
	}
	else
		month_diff = year_diff * 12 + month_diff;

	diff_usec = (uint64) ts2 - (uint64) ts1;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);
	PG_FREE_IF_COPY(units, 0);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestampdiff units \"%s\" not recognized", lowunits)));

	if (type != UNITS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("interval units \"%s\" not recognized", lowunits)));

	switch (val)
	{
		case DTK_MICROSEC:
			uval = diff_usec;
			use_uval = true;
			break;
		case DTK_SECOND:
			uval = diff_usec / (uint64) USECS_PER_SEC;
			use_uval = true;
			break;
		case DTK_MINUTE:
			uval = diff_usec / (uint64) USECS_PER_MINUTE;
			use_uval = true;
			break;
		case DTK_HOUR:
			uval = diff_usec / (uint64) USECS_PER_HOUR;
			use_uval = true;
			break;
		case DTK_DAY:
			uval = diff_usec / (uint64) USECS_PER_DAY;
			use_uval = true;
			break;
		case DTK_WEEK:
			uval = (diff_usec / (uint64) USECS_PER_DAY) / 7;
			use_uval = true;
			break;
		case DTK_MONTH:
			intresult = month_diff;
			break;
		case DTK_QUARTER:
			intresult = month_diff / 3;
			break;
		case DTK_YEAR:
			intresult = year_diff;
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("timestampdiff units \"%s\" not supported",
							lowunits)));
	}

	if (use_uval)
	{
		if (!is_negative)
		{
			if (uval > (uint64) PG_INT64_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
			intresult = (int64) uval;
		}
		else
		{
			uint64		max_neg = (uint64) PG_INT64_MAX + 1;

			if (uval > max_neg)
				ereport(ERROR,
						(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
						 errmsg("bigint out of range")));
			intresult = (uval == max_neg) ? PG_INT64_MIN : -((int64) uval);
		}
	}
	else if (is_negative)
	{
		intresult = -intresult;
	}

	PG_RETURN_INT64(intresult);
}

static TimeOffset
time2t(const int hour, const int min, const int sec, const fsec_t fsec)
{
	return (((((int64) hour * MINS_PER_HOUR + min) * SECS_PER_MINUTE) + sec) * USECS_PER_SEC) + fsec;
}

static Datum
timestamp_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	Timestamp	timestamp = PG_GETARG_TIMESTAMP(1);
	int64		intresult;
	Timestamp	epoch;
	int			type,
				val;
	char	   *lowunits;
	fsec_t		fsec;
	struct pg_tm tt,
			   *tm = &tt;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);
	PG_FREE_IF_COPY(units, 0);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (TIMESTAMP_NOT_FINITE(timestamp))
	{
		double		r = NonFiniteTimestampTzPart(type, val, lowunits,
												 TIMESTAMP_IS_NOBEGIN(timestamp),
												 false);

		if (r)
		{
			if (retnumeric)
			{
				if (r < 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("-Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
				else if (r > 0)
					return DirectFunctionCall3(numeric_in,
											   CStringGetDatum("Infinity"),
											   ObjectIdGetDatum(InvalidOid),
											   Int32GetDatum(-1));
			}
			else
				PG_RETURN_FLOAT8(r);
		}
		else
			PG_RETURN_NULL();
	}

	if (type == UNITS)
	{
		if (timestamp2tm(timestamp, NULL, tm, &fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));

		switch (val)
		{
			case DTK_MICROSEC:
				intresult = fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					PG_RETURN_NUMERIC(int64_div_fast_to_numeric(tm->tm_sec * INT64CONST(1000000) + fsec, 3));
				else
					PG_RETURN_FLOAT8(tm->tm_sec * 1000.0 + fsec / 1000.0);
				break;

			case DTK_SECOND:
				intresult = tm->tm_sec;
				break;

			case DTK_MINUTE:
				intresult = tm->tm_min;
				break;

			case DTK_HOUR:
				intresult = tm->tm_hour;
				break;

			case DTK_DAY:
				intresult = tm->tm_mday;
				break;

			case DTK_MONTH:
				intresult = tm->tm_mon;
				break;

			case DTK_QUARTER:
				intresult = (tm->tm_mon - 1) / 3 + 1;
				break;

			case DTK_WEEK:
				intresult = date2isoweek(tm->tm_year, tm->tm_mon, tm->tm_mday);
				break;

			case DTK_YEAR:
				if (tm->tm_year > 0)
					intresult = tm->tm_year;
				else
					intresult = tm->tm_year - 1;
				break;

			case DTK_DECADE:
				if (tm->tm_year >= 0)
					intresult = tm->tm_year / 10;
				else
					intresult = -((8 - (tm->tm_year - 1)) / 10);
				break;

			case DTK_CENTURY:
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 99) / 100;
				else
					intresult = -((99 - (tm->tm_year - 1)) / 100);
				break;

			case DTK_MILLENNIUM:
				if (tm->tm_year > 0)
					intresult = (tm->tm_year + 999) / 1000;
				else
					intresult = -((999 - (tm->tm_year - 1)) / 1000);
				break;

			case DTK_JULIAN:
				if (retnumeric)
					PG_RETURN_NUMERIC(numeric_add_opt_error(int64_to_numeric(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)),
															numeric_div_opt_error(int64_to_numeric(((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) + tm->tm_sec) * INT64CONST(1000000) + fsec),
																				  int64_to_numeric(SECS_PER_DAY * INT64CONST(1000000)),
																				  NULL),
															NULL));
				else
					PG_RETURN_FLOAT8(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday) +
									 ((((tm->tm_hour * MINS_PER_HOUR) + tm->tm_min) * SECS_PER_MINUTE) +
									  tm->tm_sec + (fsec / 1000000.0)) / (double) SECS_PER_DAY);
				break;

			case DTK_ISOYEAR:
				intresult = date2isoyear(tm->tm_year, tm->tm_mon, tm->tm_mday);
				if (intresult <= 0)
					intresult -= 1;
				break;

			case DTK_DOW:
			case DTK_ISODOW:
				intresult = j2day(date2j(tm->tm_year, tm->tm_mon, tm->tm_mday));
				if (val == DTK_ISODOW && intresult == 0)
					intresult = 7;
				break;

			case DTK_DOY:
				intresult = (date2j(tm->tm_year, tm->tm_mon, tm->tm_mday)
							 - date2j(tm->tm_year, 1, 1) + 1);
				break;

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}
	}
	else if (type == RESERV)
	{
		switch (val)
		{
			case DTK_EPOCH:
				epoch = SetEpochTimestamp();
				if (retnumeric)
				{
					Numeric		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = int64_div_fast_to_numeric(timestamp - epoch, 6);
					else
					{
						result = numeric_div_opt_error(numeric_sub_opt_error(int64_to_numeric(timestamp),
																			 int64_to_numeric(epoch),
																			 NULL),
													   int64_to_numeric(1000000),
													   NULL);
						result = DatumGetNumeric(DirectFunctionCall2(numeric_round,
																	 NumericGetDatum(result),
																	 Int32GetDatum(6)));
					}
					PG_RETURN_NUMERIC(result);
				}
				else
				{
					float8		result;

					if (timestamp < (PG_INT64_MAX + epoch))
						result = (timestamp - epoch) / 1000000.0;
					else
						result = ((float8) timestamp - epoch) / 1000000.0;
					PG_RETURN_FLOAT8(result);
				}
				break;

			default:
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
				intresult = 0;
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("timestamp units \"%s\" not recognized", lowunits)));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

static float8
NonFiniteTimestampTzPart(int type, int unit, char *lowunits,
						 bool isNegative, bool isTz)
{
	if ((type != UNITS) && (type != RESERV))
	{
		if (isTz)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp with time zone units \"%s\" not recognized",
							lowunits)));
		else
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("timestamp units \"%s\" not recognized",
							lowunits)));
	}

	switch (unit)
	{
		case DTK_MICROSEC:
		case DTK_MILLISEC:
		case DTK_SECOND:
		case DTK_MINUTE:
		case DTK_HOUR:
		case DTK_DAY:
		case DTK_MONTH:
		case DTK_QUARTER:
		case DTK_WEEK:
		case DTK_DOW:
		case DTK_ISODOW:
		case DTK_DOY:
		case DTK_TZ:
		case DTK_TZ_MINUTE:
		case DTK_TZ_HOUR:
			return 0.0;

		case DTK_YEAR:
		case DTK_DECADE:
		case DTK_CENTURY:
		case DTK_MILLENNIUM:
		case DTK_JULIAN:
		case DTK_ISOYEAR:
		case DTK_EPOCH:
			if (isNegative)
				return -get_float8_infinity();
			else
				return get_float8_infinity();

		default:
			if (isTz)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp with time zone units \"%s\" not supported",
								lowunits)));
			else
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("timestamp units \"%s\" not supported",
								lowunits)));
			return 0.0;
	}
}
