/*-------------------------------------------------------------------------
 *
 * mys_timestamp.h
 *	  MySQL datetime/timestamp parsing and validation helpers
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_TIMESTAMP_H
#define MYS_TIMESTAMP_H

bool mys_decodeStringDatetime(char *str, struct pg_tm *tm, fsec_t *fsec,
							  int *tzp, int flags, bool *haveTz);
bool mys_DecodeStringDatetimeForCompare(char *str, struct pg_tm *tm, fsec_t *fsec, int flags);
bool mys_strToDatetimeInternal(char *str, int flags, Timestamp *result);
bool mysCheckDate(const struct pg_tm *tm, bool not_zero_date, int flags);

#endif							/* MYS_TIMESTAMP_H */
