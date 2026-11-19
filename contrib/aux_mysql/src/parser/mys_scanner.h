/*-------------------------------------------------------------------------
 *
 * mys_scanner.h
 *	  API for the MySQL-compatible lexical scanner (mys_scan.l)
 *
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * IDENTIFICATION
 *	  contrib/aux_mysql/src/parser/mys_scanner.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_SCANNER_H
#define MYS_SCANNER_H

#include "parser/scanner.h"
#include "mys_keywords.h"

extern PGDLLIMPORT const uint16 MysScanKeywordTokens[];

/* Entry points in contrib/aux_mysql/src/parser/mys_scan.l */
extern core_yyscan_t mys_scanner_init(const char *str,
									  core_yy_extra_type *yyext,
									  const ScanKeywordList *keywordlist,
									  const uint16 *keyword_tokens);
extern core_yyscan_t mys_scanner_init_len(const char *str, Size slen,
										  core_yy_extra_type *yyext,
										  const ScanKeywordList *keywordlist,
										  const uint16 *keyword_tokens);
extern void mys_scanner_finish(core_yyscan_t yyscanner);
extern int	mys_core_yylex(core_YYSTYPE *lvalp, YYLTYPE *llocp,
						   core_yyscan_t yyscanner);
extern int	mys_scanner_errposition(int location, core_yyscan_t yyscanner);
extern void mys_setup_scanner_errposition_callback(ScannerCallbackState *scbstate,
												   core_yyscan_t yyscanner,
												   int location);
extern void mys_cancel_scanner_errposition_callback(ScannerCallbackState *scbstate);
pg_noreturn extern void mys_scanner_yyerror(const char *message, core_yyscan_t yyscanner);
extern char *mys_make_pl_block_str(core_yyscan_t yyscanner, int start);

#endif							/* MYS_SCANNER_H */
