/*-------------------------------------------------------------------------
 *
 * mys_uservar.h
 *	  MySQL session user-defined variable (@var) routines
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_USERVAR_H
#define MYS_USERVAR_H

#include "postgres.h"
#include "fmgr.h"

struct MysSetBatch;

extern void clearUserVars(void);
extern bool varValueIsDigit(Datum varValue, Oid varValueType);
extern void mysSetUserVarBytes(const char *userVarName, const char *bytes,
							   size_t byteLen, bool isDigit, bool isNull);
extern void mysSetUserVarDatum(struct MysSetBatch *batch, const char *userVarName, Datum datumValue,
							   Oid varValueType, FmgrInfo *foutcache, bool isNull);
extern void mysSetUserVarInternal(char *userVarName, char *userVarValue,
								  Oid varValueType, bool isDigit, bool isNull);
extern void mysSetUserVarForPl(char *userVarName, Datum userVarValue,
							   Oid varValueType, bool isNull);
extern bytea *mysGetUserVarValueInternal(char *userVarName);

#endif							/* MYS_USERVAR_H */
