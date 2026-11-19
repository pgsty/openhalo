/*-------------------------------------------------------------------------
 *
 * mys_keywords.c
 *	  MySQL-compatible SQL keyword lookup tables
 *
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * IDENTIFICATION
 *	  contrib/aux_mysql/src/parser/mys_keywords.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include "common/keywords.h"
#include "mys_keywords.h"

/* ScanKeywordList lookup data for SQL keywords */
#include "mys_kwlist_d.h"

/* Keyword categories for SQL keywords */
#define PG_KEYWORD(kwname, value, category, collabel) category,

const uint8 MysScanKeywordCategories[MYSSCANKEYWORDS_NUM_KEYWORDS] = {
#include "mys_kwlist.h"
};

#undef PG_KEYWORD
