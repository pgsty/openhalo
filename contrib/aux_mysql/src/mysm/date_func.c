/*-------------------------------------------------------------------------
 *
 * date_func.c
 *	  MySQL EXTRACT() time and interval conversion helpers
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "parser/scansup.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/numeric.h"

static Datum time_part_common(PG_FUNCTION_ARGS, bool retnumeric);


PG_FUNCTION_INFO_V1(mys_extract_time);

Datum
mys_extract_time(PG_FUNCTION_ARGS)
{
	return time_part_common(fcinfo, true);
}

PG_FUNCTION_INFO_V1(text_interval);

/* text_interval()
 * Convert text to interval data type.
 */
Datum
text_interval(PG_FUNCTION_ARGS)
{
	/* PG18: same as interval_in() on the text value */
	char	   *str = TextDatumGetCString(PG_GETARG_DATUM(0));
	int32		typmod = PG_GETARG_INT32(1);
	Datum		res;

	res = DirectFunctionCall3(interval_in, CStringGetDatum(str),
							  ObjectIdGetDatum(InvalidOid),
							  Int32GetDatum(typmod));
	pfree(str);
	return res;
}

/* time_part_common()
 * Extract specified field from time type.
 */
static Datum
time_part_common(PG_FUNCTION_ARGS, bool retnumeric)
{
	text	   *units = PG_GETARG_TEXT_PP(0);
	TimeADT		time = PG_GETARG_TIMEADT(1);
	int64		intresult;
	int			type,
				val;
	char	   *lowunits;

	lowunits = downcase_truncate_identifier(VARDATA_ANY(units),
											VARSIZE_ANY_EXHDR(units),
											false);

	type = DecodeUnits(0, lowunits, &val);
	if (type == UNKNOWN_FIELD)
		type = DecodeSpecial(0, lowunits, &val);

	if (type == UNITS)
	{
		fsec_t		fsec;
		struct pg_tm tt,
				   *tm = &tt;

		time2tm(time, tm, &fsec);

		switch (val)
		{
			case DTK_MICROSEC:
				intresult = fsec;
				break;

			case DTK_MILLISEC:
				if (retnumeric)
					/*---
					 * tm->tm_sec * 1000 + fsec / 1000
					 * = (tm->tm_sec * 1'000'000 + fsec) / 1000
					 */
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

			case DTK_TZ:
			case DTK_TZ_MINUTE:
			case DTK_TZ_HOUR:
			case DTK_DAY:
			case DTK_MONTH:
			case DTK_QUARTER:
			case DTK_YEAR:
			case DTK_DECADE:
			case DTK_CENTURY:
			case DTK_MILLENNIUM:
			case DTK_ISOYEAR:
			default:
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("\"time\" units \"%s\" not recognized",
								lowunits)));
				intresult = 0;
		}
	}
	else if (type == RESERV && val == DTK_EPOCH)
	{
		if (retnumeric)
			PG_RETURN_NUMERIC(int64_div_fast_to_numeric(time, 6));
		else
			PG_RETURN_FLOAT8(time / 1000000.0);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("\"time\" units \"%s\" not recognized",
						lowunits)));
		intresult = 0;
	}

	if (retnumeric)
		PG_RETURN_NUMERIC(int64_to_numeric(intresult));
	else
		PG_RETURN_FLOAT8(intresult);
}

