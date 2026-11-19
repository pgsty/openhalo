/*-------------------------------------------------------------------------
 *
 * mys_date.h
 *	  MySQL date/time parsing constants and helpers
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_DATE_H
#define MYS_DATE_H

#include "fmgr.h"
#include "utils/date.h"

/* year, month, day, hour, minute, second, microsecond, timezone */
#define MYS_MAX_TIMESTAMP_PARTS 8
#define MYS_MAX_TIME_ZONE_HOURS 14
#define MYS_MAX_TIME_HOUR 838

#define MYS_TIME_FUZZY_DATE 1
#define MYS_TIME_DATETIME_ONLY 2
#define MYS_TIME_NO_NSEC_ROUNDING 4
#define MYS_TIME_NO_DATE_FRAC_WARN 8
#define MYS_TIME_NO_ZERO_IN_DATE 16
#define MYS_TIME_NO_ZERO_DATE 32
#define MYS_TIME_INVALID_DATES 64

void mys_pre_time_in_for_subtime(char *str, struct pg_tm *tm, fsec_t *fsec, int *sign);

#endif							/* MYS_DATE_H */
