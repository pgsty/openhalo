/*-------------------------------------------------------------------------
 *
 * mys_date.c
 *	  MySQL time and datetime string preprocessing for SUBTIME/ADDTIME/TIMEDIFF
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
#include <limits.h>

#include "datatype/timestamp.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "pgtime.h"
#include "utils/date.h"
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
static char *mysPreTimeInInternal(char *str);
static char *mysProcessStringTimeForFormat1(char *str);
static char *mysProcessStringTimeForFormat2(char *str);
static char *mysProcessStringTimeForFormat3(char *str);
static char *mysPreTimestampInForTime(char *str, bool *isTimestamp);

void
mys_pre_time_in_for_subtime(char *str, struct pg_tm *tm, fsec_t *fsec, int *sign)
{
	int			tz;
	int			nf;
	int			dterr;
	DateTimeErrorExtra dterr_extra;
	char		workbuf[MAXDATELEN + 1];
	char	   *field[MAXDATEFIELDS];
	int			dtype;
	int			ftype[MAXDATEFIELDS];

	*sign = 1;

	str = mysPreTimeInInternal(str);
	if (str == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_DATETIME_FORMAT),
				 errmsg("Incorrect time value.")));

	if (str[0] == '-')
	{
		*sign = -1;
		str = str + 1;
	}

	dterr = ParseDateTime(str, workbuf, sizeof(workbuf),
						  field, ftype, MAXDATEFIELDS, &nf);
	if (dterr == 0)
		dterr = DecodeTimeOnly(field, ftype, nf, &dtype, tm, fsec, &tz, &dterr_extra);
	if (dterr != 0)
		DateTimeParseError(dterr, &dterr_extra, str, "time", NULL);

	if (tm->tm_year > 0)
		*sign = 1;
}

static char *
mysPreTimeInInternal(char *str)
{
	char	   *result;
	char	   *tempResult;
	bool		nega = false;
	int			startIndex;
	int			strIndex;
	int			strLen = strlen(str);
	uint64		value;
	int			numOfSpaceAfterDay;

	for (strIndex = 0; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			break;
	}

	if (strIndex < strLen && str[strIndex] == '-')
	{
		nega = true;
		strIndex++;
	}

	if (strIndex == strLen)
		return NULL;

	startIndex = strIndex;
	result = palloc0(64);

	if ((strLen - strIndex) >= 12)
	{
		bool		isTimestamp = false;

		tempResult = mysPreTimestampInForTime(str + strIndex, &isTimestamp);
		if (isTimestamp)
		{
			if (tempResult != NULL)
			{
				pg_snprintf(result, 64, nega ? "-%s" : "%s", tempResult);
				pfree(tempResult);
				return result;
			}
			pfree(result);
			return NULL;
		}
	}

	/* Not a timestamp. Try to get this as a DAYS_TO_SECOND string */
	for (value = 0; strIndex < strLen && isdigit((unsigned char) str[strIndex]); strIndex++)
	{
		value = value * 10 + (uint64) (str[strIndex] - '0');
		if (value > UINT_MAX)
		{
			pfree(result);
			return NULL;
		}
	}

	for (numOfSpaceAfterDay = 0; strIndex < strLen; strIndex++)
	{
		if (isspace((unsigned char) str[strIndex]))
			numOfSpaceAfterDay++;
		else
			break;
	}

	if (numOfSpaceAfterDay > 0)
	{
		if (strIndex < strLen)
		{
			if (isdigit((unsigned char) str[strIndex]))
				tempResult = mysProcessStringTimeForFormat1(str + startIndex);
			else if (str[strIndex] == ':')
				tempResult = mysProcessStringTimeForFormat2(str + startIndex);
			else
				tempResult = mysProcessStringTimeForFormat3(str + startIndex);
		}
		else
			tempResult = mysProcessStringTimeForFormat3(str + startIndex);
	}
	else
	{
		if (strIndex < strLen && str[strIndex] == ':')
			tempResult = mysProcessStringTimeForFormat2(str + startIndex);
		else
			tempResult = mysProcessStringTimeForFormat3(str + startIndex);
	}

	if (tempResult == NULL)
	{
		pfree(result);
		return NULL;
	}

	pg_snprintf(result, 64, nega ? "-%s" : "%s", tempResult);
	pfree(tempResult);
	return result;
}

/*
 * 'D hh:mm:ss[.fsec]' | 'D hh:mm' | 'D hh'
 */
static char *
mysProcessStringTimeForFormat1(char *str)
{
	char	   *result = NULL;
	int			strIndex = 0;
	int			strLen = strlen(str);
	uint64		data[5] = {0};
	int			dataIndex = 0;
	int			nanoSecond = 0;

	if (strLen == 0 || str[0] == ' ')
		return NULL;

	for (; dataIndex < 4; dataIndex++)
	{
		uint64		tempValue = 0;

		for (; strIndex < strLen; strIndex++)
		{
			unsigned char cha = (unsigned char) str[strIndex];

			if (isdigit(cha))
			{
				tempValue = tempValue * 10 + (cha - '0');
				if (tempValue > 999999999ULL)
					return NULL;
			}
			else
				break;
		}
		data[dataIndex] = tempValue;

		if (strIndex >= strLen)
			break;

		if (dataIndex == 0)
		{
			if (!isspace((unsigned char) str[strIndex]))
				return NULL;
			while (strIndex < strLen && isspace((unsigned char) str[strIndex]))
				strIndex++;
		}
		else if (dataIndex == 3)
			break;
		else
		{
			if (str[strIndex] != ':')
				break;
			if (strIndex + 1 < strLen && isdigit((unsigned char) str[strIndex + 1]))
				strIndex++;
			else
				break;
		}
	}

	if (strIndex < strLen && str[strIndex] == '.')
	{
		int			fsec = 0;
		int			fsecLen = 0;

		strIndex++;
		for (; strIndex < strLen; strIndex++)
		{
			unsigned char cha = (unsigned char) str[strIndex];

			if (fsecLen >= MAX_TIMESTAMP_PRECISION)
				break;
			if (isdigit(cha))
			{
				fsec = fsec * 10 + (cha - '0');
				fsecLen++;
			}
			else
				break;
		}

		if (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
		{
			nanoSecond = str[strIndex] - '0';
			strIndex++;
			while (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
				strIndex++;
		}

		data[4] = fsec * log_10_int[MAX_TIMESTAMP_PRECISION - fsecLen];
	}

	for (; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			return NULL;
	}

	if ((data[0] * 24 + data[1]) > MYS_MAX_TIME_HOUR ||
		data[2] > 59 || data[3] > 59 || data[4] > 999999)
		return NULL;

	result = palloc0(64);
	pg_snprintf(result, 64, "%u:%02u:%02u.%06u%u",
				(uint) (data[0] * 24 + data[1]), (uint) data[2], (uint) data[3],
				(uint) data[4], (uint) nanoSecond);
	return result;
}

/*
 * 'hh:mm:ss[.fsec]' | 'hh :mm:ss[.fsec]' | 'hh:mm[.fsec]' | 'hh :'
 */
static char *
mysProcessStringTimeForFormat2(char *str)
{
	char	   *result = NULL;
	int			strIndex = 0;
	int			strLen = strlen(str);
	uint64		data[4] = {0};
	int			dataIndex = 0;
	int			nanoSecond = 0;

	if (strLen == 0 || str[0] == ' ')
		return NULL;

	for (; dataIndex < 3; dataIndex++)
	{
		uint64		tempValue = 0;

		for (; strIndex < strLen; strIndex++)
		{
			unsigned char cha = (unsigned char) str[strIndex];

			if (isdigit(cha))
			{
				tempValue = tempValue * 10 + (cha - '0');
				if (tempValue > 999999999ULL)
					return NULL;
			}
			else
				break;
		}
		data[dataIndex] = tempValue;

		if (strIndex >= strLen)
			break;

		if (dataIndex == 0)
		{
			while (strIndex < strLen && isspace((unsigned char) str[strIndex]))
				strIndex++;
			if (strIndex >= strLen || str[strIndex] != ':')
				break;
			strIndex++;
		}
		else if (dataIndex == 1)
		{
			if (str[strIndex] != ':')
				break;
			strIndex++;
		}
		else
			break;
	}

	if (strIndex < strLen && str[strIndex] == '.')
	{
		int			fsec = 0;
		int			fsecLen = 0;

		strIndex++;
		for (; strIndex < strLen; strIndex++)
		{
			unsigned char cha = (unsigned char) str[strIndex];

			if (fsecLen >= MAX_TIMESTAMP_PRECISION)
				break;
			if (isdigit(cha))
			{
				fsec = fsec * 10 + (cha - '0');
				fsecLen++;
			}
			else
				break;
		}

		if (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
		{
			nanoSecond = str[strIndex] - '0';
			strIndex++;
			while (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
				strIndex++;
		}

		data[3] = fsec * log_10_int[MAX_TIMESTAMP_PRECISION - fsecLen];
	}

	for (; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			return NULL;
	}

	if (data[0] > MYS_MAX_TIME_HOUR || data[1] > 59 || data[2] > 59 || data[3] > 999999)
		return NULL;

	result = palloc0(64);
	pg_snprintf(result, 64, "%u:%02u:%02u.%06u%u",
				(uint) data[0], (uint) data[1], (uint) data[2],
				(uint) data[3], (uint) nanoSecond);
	return result;
}

/*
 * 'hhmmss[.fsec]'
 */
static char *
mysProcessStringTimeForFormat3(char *str)
{
	char	   *result = NULL;
	int			strIndex = 0;
	int			strLen = strlen(str);
	uint64		data[4] = {0};
	int			nanoSecond = 0;
	uint64		value = 0;

	if (strLen == 0 || str[0] == ' ')
		return NULL;

	for (; strIndex < strLen; strIndex++)
	{
		unsigned char cha = (unsigned char) str[strIndex];

		if (isdigit(cha))
		{
			value = value * 10 + (cha - '0');
			if (value > 999999999ULL)
				return NULL;
		}
		else
			break;
	}

	data[0] = value / 10000;
	data[1] = (value / 100) % 100;
	data[2] = value % 100;

	while (strIndex < strLen && isspace((unsigned char) str[strIndex]))
		strIndex++;

	if (strIndex < strLen && str[strIndex] == '.')
	{
		int			fsec = 0;
		int			fsecLen = 0;

		strIndex++;
		for (; strIndex < strLen; strIndex++)
		{
			unsigned char cha = (unsigned char) str[strIndex];

			if (fsecLen >= MAX_TIMESTAMP_PRECISION)
				break;
			if (isdigit(cha))
			{
				fsec = fsec * 10 + (cha - '0');
				fsecLen++;
			}
			else
				break;
		}

		if (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
		{
			nanoSecond = str[strIndex] - '0';
			strIndex++;
			while (strIndex < strLen && isdigit((unsigned char) str[strIndex]))
				strIndex++;
		}

		data[3] = fsec * log_10_int[MAX_TIMESTAMP_PRECISION - fsecLen];
	}

	for (; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			return NULL;
	}

	if (data[0] > MYS_MAX_TIME_HOUR || data[1] > 59 || data[2] > 59 || data[3] > 999999)
		return NULL;

	result = palloc0(64);
	pg_snprintf(result, 64, "%u:%02u:%02u.%06u%u",
				(uint) data[0], (uint) data[1], (uint) data[2],
				(uint) data[3], (uint) nanoSecond);
	return result;
}

static char *
mysPreTimestampInForTime(char *str, bool *isTimestamp)
{
	struct pg_tm tt;
	struct pg_tm *tm = &tt;
	char	   *result;
	int			strIndex;
	int			strLen = strlen(str);
	int			saveIndex;
	int			yearLen = 0;
	int			numberOfParts;
	int			currentPartLen;
	int			tsPartIndex;
	int			tsParts[MYS_MAX_TIMESTAMP_PARTS] = {0};
	int			tsPartsLen[MYS_MAX_TIMESTAMP_PARTS] = {0};
	bool		isInternalFormat = false;
	int			tzDisplacement = 0;
	bool		foundTzDisplacement = false;
	fsec_t		fsec = 0;
	int			nanoseconds = 0;
	bool		foundDelimiter = false;
	bool		foundSpace = false;
	int			allowSpace;
	uint64		notZeroDate;

	memset(tm, 0, sizeof(*tm));

	for (strIndex = 0; strIndex < strLen; strIndex++)
	{
		if (!isspace((unsigned char) str[strIndex]))
			break;
	}

	if (strIndex == strLen || !isdigit((unsigned char) str[strIndex]))
		goto error1;

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
				goto error1;
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
					goto error1;
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
					goto error1;
			}
		}

		while (strIndex < strLen &&
			   (ispunct((unsigned char) str[strIndex]) ||
				isspace((unsigned char) str[strIndex])))
		{
			if (isspace((unsigned char) str[strIndex]))
			{
				if (!(allowSpace & (1 << tsPartIndex)))
					goto error1;
				foundSpace = true;
			}
			strIndex++;
			foundDelimiter = true;
		}
	}

	if (foundDelimiter && !foundSpace)
		goto error1;

	if (!isInternalFormat)
	{
		yearLen = tsPartsLen[0];
		if (!yearLen)
			goto error1;
	}

	tm->tm_year = tsParts[0];
	tm->tm_mon = tsParts[1];
	tm->tm_mday = tsParts[2];
	tm->tm_hour = tsParts[3];
	tm->tm_min = tsParts[4];
	tm->tm_sec = tsParts[5];
	if (tsPartsLen[6] <= 6)
		fsec = tsParts[6] * log_10_int[MAX_TIMESTAMP_PRECISION - tsPartsLen[6]];

	if (yearLen == 2 && notZeroDate)
		tm->tm_year += (tm->tm_year < 70 ? 2000 : 1900);

	*isTimestamp = true;

	if (numberOfParts < 3 || !mysCheckTimestampRange(tm, fsec))
		goto error2;

	if (tsPartsLen[6] == 6 && strIndex < strLen && isdigit((unsigned char) str[strIndex]))
	{
		nanoseconds = str[strIndex] - '0';
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
				*isTimestamp = true;
				foundTzDisplacement = true;
				strIndex = strLen;
			}
			else
				goto error1;
		}
		else
		{
			for (; strIndex < strLen; strIndex++)
			{
				if (!isspace((unsigned char) str[strIndex]))
					goto error2;
			}
		}
	}

	if (foundTzDisplacement)
	{
		Timestamp	ts;
		int			tz = tzDisplacement;

		if (nanoseconds >= 5)
			fsec++;
		nanoseconds = 0;

		if (!mysCheckDate(tm, notZeroDate != 0,
						  MYS_TIME_NO_ZERO_IN_DATE | MYS_TIME_NO_ZERO_DATE))
			goto error2;
		if (tm2timestamp(tm, fsec, &tz, &ts) != 0)
			goto error2;
		if (timestamp2tm(ts, &tz, tm, &fsec, NULL, session_timezone) != 0)
			goto error2;
	}

	result = palloc0(64);
	if (fsec != 0 || nanoseconds != 0)
		pg_snprintf(result, 64, "%04d-%02d-%02d %02d:%02d:%02d.%06d%d",
					tm->tm_year, tm->tm_mon, tm->tm_mday,
					tm->tm_hour, tm->tm_min, tm->tm_sec,
					fsec, nanoseconds);
	else
		pg_snprintf(result, 64, "%04d-%02d-%02d %02d:%02d:%02d",
					tm->tm_year, tm->tm_mon, tm->tm_mday,
					tm->tm_hour, tm->tm_min, tm->tm_sec);

	return result;

error1:
	*isTimestamp = false;
	return NULL;
error2:
	*isTimestamp = true;
	return NULL;
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

	/* PostgreSQL tz offset convention: positive west of Greenwich */
	*result = -(seconds * sign);
	return true;
}
