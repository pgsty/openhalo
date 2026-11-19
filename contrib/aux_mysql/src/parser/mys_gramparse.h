/*-------------------------------------------------------------------------
 *
 * mys_gramparse.h
 *	  Shared definitions for the MySQL-compatible bison parser and scanner
 *
 * Portions Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 * IDENTIFICATION
 *	  contrib/aux_mysql/src/parser/mys_gramparse.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef MYS_GRAMPARSE_H
#define MYS_GRAMPARSE_H

#include "nodes/parsenodes.h"
#include "mys_parsenodes.h"
#include "mys_scanner.h"

/* MUST INCLUDE AFTER mys_scanner.h */
#include "mys_gram.h"

typedef struct mys_yy_extra_type
{
	/*
	 * Fields used by the core scanner.
	 */
	core_yy_extra_type core_yy_extra;

	/*
	 * State variables for mys_yylex().
	 */
	bool		have_lookahead; /* is lookahead info valid? */
	int			lookahead_token;	/* one-token lookahead */
	core_YYSTYPE lookahead_yylval;	/* yylval for lookahead token */
	YYLTYPE		lookahead_yylloc;	/* yylloc for lookahead token */
	char	   *lookahead_end;	/* end of current token */
	char		lookahead_hold_char;	/* to be put back at *lookahead_end */
	int			prev_token;		/* previous token emitted by mys_yylex() */
	int			on_dup_state;	/* 0..4: inside ON DUPLICATE KEY UPDATE */
	char	   *pending_use_db; /* most recent USE db in current multi-stmt */

	/*
	 * State variables that belong to the grammar.
	 */
	List	   *parsetree;		/* final parse result is delivered here */

	/*
	 * MySQL scanner state.  openHalo kept this in core_yy_extra_type; we
	 * cannot extend that core struct, so the scanner reaches it through
	 * MYS_YYEXTRA(), relying on core_yy_extra being the first member.
	 */
	bool		saw_underscore_binary;	/* saw a _binary introducer */
	int			binary_token_end;	/* end of a decoded binary string, or 0 */
	bool		normalizing;	/* scanning for mys_normalize_query */
	bool		in_exec_comment;	/* inside a MySQL executable comment */
	bool		xc_version_skip;	/* skipping a newer-version executable comment */
} mys_yy_extra_type;

#define MYS_YYEXTRA(core_extra) ((mys_yy_extra_type *) (core_extra))

extern int	mys_yylex(YYSTYPE *lvalp, YYLTYPE *llocp,
					  core_yyscan_t yyscanner);

#define mys_yyget_extra(yyscanner) (*((mys_yy_extra_type **) (yyscanner)))

extern void mys_parser_init(mys_yy_extra_type *yyext);
extern int	mys_yyparse(core_yyscan_t yyscanner);

/* mys_binary.c: hexadecimal literals are mysql.hexlit constants */
extern bool mys_hexlit_available;

#endif							/* MYS_GRAMPARSE_H */
