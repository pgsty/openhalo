/*-------------------------------------------------------------------------
 *
 * mys_keywords.h
 *	  Lexical token lookup for MySQL-compatible SQL keywords
 *
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * IDENTIFICATION
 *	  contrib/aux_mysql/src/parser/mys_keywords.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_KEYWORDS_H
#define MYS_KEYWORDS_H

#include "common/kwlookup.h"

#ifndef FRONTEND
extern PGDLLIMPORT const ScanKeywordList MysScanKeywords;
extern PGDLLIMPORT const uint8 MysScanKeywordCategories[];
#else
extern const ScanKeywordList MysScanKeywords;
extern const uint8 MysScanKeywordCategories[];
#endif

#endif							/* MYS_KEYWORDS_H */
