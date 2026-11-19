/*-------------------------------------------------------------------------
 *
 * mys_timestamp.c
 *	  MySQL datetime/timestamp parsing and validation helpers
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include <ctype.h>

#include "datatype/timestamp.h"
#include "pgtime.h"
#include "utils/datetime.h"
#include "utils/mysql/mys_date.h"
#include "utils/mysql/mys_timestamp.h"

static const int64 log_10_int[MAX_TIMESTAMP_PRECISION + 1] = {
	INT64CONST(1),
	INT64CONST(10),
	INT64CONST(100),
	INT64CONST(1000),
	INT64CONST(10000),
	INT64CONST(100000),
	INT64CONST(1000000)
};

static bool mysCheckTimestampRange(struct pg_tm *tm, fsec_t fsec);
static bool tzDisplacement2Seconds(const char *str, int length, int *result);

bool
mys_strToDatetimeInternal(char *str, int flags, Timestamp *result)
{
	struct pg_tm tt;
	struct pg_tm *tm = &tt;
	fsec_t		fsec = 0;
	int			tz = 0;
	bool		haveTz = false;

	memset(tm, 0, sizeof(*tm));

	if (mys_decodeStringDatetime(str, tm, &fsec, &tz, flags, &haveTz))
	{
		if (fsec == USECS_PER_SEC)
		{
			if (!mysCheckDate(tm, true, MYS_TIME_NO_ZERO_IN_DATE | MYS_TIME_NO_ZERO_DATE))
				return false;
		}
	}
	else
		return false;

	if (haveTz)
	{
		if (tm2timestamp(tm, fsec, &tz, result) != 0)
			return false;

		if (timestamp2tm(*result, &tz, tm, &fsec, NULL, session_timezone) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
		if (tm2timestamp(tm, fsec, NULL, result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}
	else
	{
		if (tm2timestamp(tm, fsec, NULL, result) != 0)
			return false;
	}

	return true;
}

bool
mys_DecodeStringDatetimeForCompare(char *str, struct pg_tm *tm, fsec_t *fsec, int flags)
{
	int			tz = 0;
	bool		haveTz = false;

	if (!mys_decodeStringDatetime(str, tm, fsec, &tz, flags, &haveTz))
		return false;

	if (haveTz)
	{
		Timestamp	ts;

		if (!mysCheckDate(tm, true, MYS_TIME_NO_ZERO_IN_DATE | MYS_TIME_NO_ZERO_DATE))
			return false;
		if (tm2timestamp(tm, *fsec, &tz, &ts) != 0)
			return false;
		if (timestamp2tm(ts, &tz, tm, fsec, NULL, session_timezone) != 0)
			return false;
		return true;
	}

	if (*fsec == USECS_PER_SEC)
	{
		Timestamp	result;

		if (!mysCheckDate(tm, true, MYS_TIME_NO_ZERO_IN_DATE | MYS_TIME_NO_ZERO_DATE))
			return false;

		if (tm2timestamp(tm, *fsec, NULL, &result) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range: \"%s\"", str)));

		if (timestamp2tm(result, NULL, tm, fsec, NULL, NULL) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_DATETIME_VALUE_OUT_OF_RANGE),
					 errmsg("timestamp out of range")));
	}

	return true;
}

bool
mys_decodeStringDatetime(char *str, struct pg_tm *tm, fsec_t *fsec, int *tzp, int flags, bool *haveTz)
{
	int			strIndex;
	int			saveIndex;
	int			yearLen = 0;
	int			numberOfParts;
	int			currentPartLen;
	int			tsPartIndex;
	int			tsParts[MYS_MAX_TIMESTAMP_PARTS] = {0};
	int			tsPartsLen[MYS_MAX_TIMESTAMP_PARTS] = {0};
	bool		isInternalFormat = false;
	int			strLen = strlen(str);
	int			tzDisplacement = 0;
	bool		foundTzDisplacement = false;
	int			allowSpace;
	uint64		notZeroDate;

	*fsec = 0;

	for (strIndex = 0; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			break;
	}

	if (strIndex == strLen || !isdigit((unsigned char) str[strIndex]))
		goto error;

	saveIndex = strIndex;
	for (; strIndex < strLen; strIndex++)
	{
		unsigned char cha = (unsigned char) str[strIndex];

		if (isdigit(cha) || cha == 'T')
			continue;
		break;
	}

	if (strIndex == strLen || str[strIndex] == '.')
	{
		int			numLen = strIndex - saveIndex;

		yearLen = (numLen == 4 || numLen == 8 || numLen >= 14) ? 4 : 2;
		currentPartLen = yearLen;
		isInternalFormat = true;
	}
	else
		currentPartLen = 4;

	strIndex = saveIndex;
	allowSpace = ((1 << 2) | (1 << 6));
	notZeroDate = 0;
	numberOfParts = 0;

	for (tsPartIndex = 0;
		 tsPartIndex < MYS_MAX_TIMESTAMP_PARTS - 1 &&
		 strIndex < strLen &&
		 isdigit((unsigned char) str[strIndex]);
		 tsPartIndex++)
	{
		int			currentPartStartIndex = strIndex;
		int			tempPartValue = 0;
		bool		scanUntilDelim = !isInternalFormat && (tsPartIndex != 6);

		while (strIndex < strLen && isdigit((unsigned char) str[strIndex]) &&
			   (scanUntilDelim || currentPartLen--))
		{
			tempPartValue = tempPartValue * 10 + (int) (str[strIndex] - '0');
			strIndex++;
			if (tempPartValue > 999999)
				goto error;
		}
		tsPartsLen[tsPartIndex] = strIndex - currentPartStartIndex;
		tsParts[tsPartIndex] = tempPartValue;
		notZeroDate |= tempPartValue;
		numberOfParts++;

		currentPartLen = 2;

		if (strIndex == strLen)
			break;

		if (tsPartIndex == 2 && str[strIndex] == 'T')
		{
			strIndex++;
			continue;
		}
		else if (tsPartIndex == 5)
		{
			if (str[strIndex] == '.')
			{
				strIndex++;
				currentPartLen = 6;
			}
			else if (isdigit((unsigned char) str[strIndex]))
				break;
			else if (str[strIndex] == '+' || str[strIndex] == '-')
			{
				if (tzDisplacement2Seconds(str + strIndex, strLen - strIndex, &tzDisplacement))
				{
					foundTzDisplacement = true;
					strIndex = strLen;
				}
				else
					goto error;
			}
			continue;
		}
		else if (tsPartIndex == 6)
		{
			if (str[strIndex] == '+' || str[strIndex] == '-')
			{
				if (tzDisplacement2Seconds(str + strIndex, strLen - strIndex, &tzDisplacement))
				{
					foundTzDisplacement = true;
					strIndex = strLen;
				}
				else
					goto error;
			}
		}

		while (strIndex < strLen &&
			   (ispunct((unsigned char) str[strIndex]) ||
				isspace((unsigned char) str[strIndex])))
		{
			if (isspace((unsigned char) str[strIndex]))
			{
				if (!(allowSpace & (1 << tsPartIndex)))
					goto error;
			}
			strIndex++;
		}
	}

	if (!isInternalFormat)
	{
		yearLen = tsPartsLen[0];
		if (!yearLen)
			goto error;
	}

	tm->tm_year = tsParts[0];
	tm->tm_mon = tsParts[1];
	tm->tm_mday = tsParts[2];
	tm->tm_hour = tsParts[3];
	tm->tm_min = tsParts[4];
	tm->tm_sec = tsParts[5];
	if (tsPartsLen[6] <= 6)
		*fsec = tsParts[6] * log_10_int[MAX_TIMESTAMP_PRECISION - tsPartsLen[6]];

	if (yearLen == 2 && notZeroDate)
		tm->tm_year += (tm->tm_year < 70 ? 2000 : 1900);

	if (numberOfParts < 3 || !mysCheckTimestampRange(tm, *fsec))
		goto error;

	if (!mysCheckDate(tm, notZeroDate != 0, flags))
		goto error;

	if (tsPartsLen[6] == 6 && strIndex < strLen && isdigit((unsigned char) str[strIndex]))
	{
		*fsec += ((str[strIndex] - '0' >= 5) ? 1 : 0);
		strIndex++;
		while (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
			strIndex++;
	}

	if (strIndex < strLen)
	{
		if (str[strIndex] == '+' || str[strIndex] == '-')
		{
			if (tzDisplacement2Seconds(str + strIndex, strLen - strIndex, &tzDisplacement))
			{
				foundTzDisplacement = true;
				strIndex = strLen;
			}
			else
				goto error;
		}
		else
		{
			for (; strIndex < strLen; strIndex++)
			{
				if (!isspace((unsigned char) str[strIndex]))
					goto error;
			}
		}
	}

	if (foundTzDisplacement)
	{
		*tzp = tzDisplacement;
		*haveTz = true;
	}
	else
	{
		*tzp = 0;
		*haveTz = false;
	}

	return true;

error:
	return false;
}

static bool
mysCheckTimestampRange(struct pg_tm *tm, fsec_t fsec)
{
	return (uint32) tm->tm_year <= 9999U &&
		(uint32) tm->tm_mon <= 12U &&
		(uint32) tm->tm_mday <= 31U &&
		(uint32) tm->tm_hour <= 23U &&
		(uint32) tm->tm_min <= 59U &&
		(uint32) tm->tm_sec <= 59U &&
		(uint32) fsec <= 999999U;
}

bool
mysCheckDate(const struct pg_tm *tm, bool not_zero_date, int flags)
{
	if (not_zero_date)
	{
		if (((flags & MYS_TIME_NO_ZERO_IN_DATE) || !(flags & MYS_TIME_FUZZY_DATE)) &&
			(tm->tm_mon == 0 || tm->tm_mday == 0))
			return false;
		if (!(flags & MYS_TIME_INVALID_DATES) &&
			tm->tm_mon >= 1 && tm->tm_mon <= 12 &&
			tm->tm_mday > day_tab[isleap(tm->tm_year)][tm->tm_mon - 1])
			return false;
	}
	else if (flags & MYS_TIME_NO_ZERO_DATE)
	{
		return false;
	}

	return true;
}

static bool
tzDisplacement2Seconds(const char *str, int length, int *result)
{
	int			sign;
	int			hours;
	int			minutes;
	int			seconds;
	int			i;

	if (length < 6)
		return false;

	sign = (str[0] == '+') ? 1 : ((str[0] == '-') ? -1 : 0);
	if (sign == 0)
		return false;

	if (!(isdigit((unsigned char) str[1]) && isdigit((unsigned char) str[2])))
		return false;
	hours = (str[1] - '0') * 10 + (str[2] - '0');

	if (str[3] != ':')
		return false;

	if (!(isdigit((unsigned char) str[4]) && isdigit((unsigned char) str[5])))
		return false;
	minutes = (str[4] - '0') * 10 + (str[5] - '0');
	if (minutes >= MINS_PER_HOUR)
		return false;

	seconds = hours * SECS_PER_HOUR + minutes * SECS_PER_MINUTE;
	if (seconds > MYS_MAX_TIME_ZONE_HOURS * SECS_PER_HOUR)
		return false;

	if (sign == -1 && hours == 0 && minutes == 0)
		return false;

	for (i = 6; i < length; ++i)
	{
		if (!isspace((unsigned char) str[i]))
			return false;
	}

	*result = -(seconds * sign);
	return true;
}
