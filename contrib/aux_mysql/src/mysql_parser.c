/*-------------------------------------------------------------------------
 *
 * mysql_parser.c
 *	  Entry point of the MySQL dialect parser.
 *
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "parser/parser.h"
#include "parser/scansup.h"
#include "mb/pg_wchar.h"
#include "lib/stringinfo.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "mys_gramparse.h"

#include "halo_mysql.h"
#include "adapter/mysql/systemVar.h"

#ifdef HALO_MYSQL_GRAMMAR
extern List *mys_raw_parser(const char *str);
#endif

/*
 * A conservative byte filter. The lexer distinguishes escapes from trail
 * bytes in client encodings such as SJIS.
 */
static bool
has_nul_escape(const char *query, int len)
{
	if (mys_sqlMode & MYS_MODE_NO_BACKSLASH_ESCAPES)
		return false;
	for (int i = 0; i + 1 < len; i++)
	{
		if (query[i] == '\\' && query[i + 1] == '0')
			return true;
	}
	return false;
}

/*
 * PostgreSQL validates COM_QUERY as text before invoking the parser hook, and
 * a PostgreSQL string cannot hold a NUL.  MySQL keeps the bytes of a string
 * literal as they are, whatever they are, so use our actual lexer to rewrite
 * as X'hex' every string literal that PostgreSQL could not take as text: the
 * _binary ones, and plain ones holding a NUL (raw or \0) or bytes that are not
 * valid in the database encoding (a plain one becomes _binary X'hex', so it
 * reaches a BLOB column unchanged and a TEXT column through the encoding
 * check of the bytea -> text cast, as in MySQL's strict mode).  Comments,
 * identifiers and the other strings still undergo normal validation, and
 * keeping one lexer keeps quoting and SQL mode interpretation identical.
 */
char *
mys_normalize_query(const char *query, int len, int encoding)
{
	mys_yy_extra_type extra;
	core_yyscan_t scanner;
	core_YYSTYPE value;
	YYLTYPE loc = 0;
	StringInfoData out;
	int copied = 0;
	int			prev_token = 0;
	int			end = len;

	/* SQL_ASCII clients leave validation of ordinary text to the database. */
	if (encoding == PG_SQL_ASCII)
		encoding = GetDatabaseEncoding();

	/* as in MySQL, a NUL at the end (a C string sent with its terminator) ends the query */
	while (end > 0 && (query[end - 1] == '\0' || scanner_isspace(query[end - 1])))
		end--;
	if (memchr(query + end, '\0', len - end))
		len = end;

	if (halo_mysql_parser == MYS_PARSER_POSTGRES)
	{
		/* PostgreSQL's lexer reads the text; it can only take valid text */
		if (memchr(query, '\0', len))
			ereport(ERROR, (errcode(ERRCODE_CHARACTER_NOT_IN_REPERTOIRE),
							errmsg("NUL in query text; use a binary parameter or hexadecimal literal")));
		return pnstrdup(query, len);
	}
	if (memchr(query, '\0', len) == NULL && !has_nul_escape(query, len) &&
		(encoding == GetDatabaseEncoding() || memchr(query, '_', len) == NULL) &&
		pg_verify_mbstr(encoding, query, len, true))
		return pnstrdup(query, len);
	memset(&extra, 0, sizeof(extra));
	scanner = mys_scanner_init_len(query, len, &extra.core_yy_extra,
								   &MysScanKeywords, MysScanKeywordTokens);
	extra.normalizing = true;
	extra.literal_encoding = encoding;
	if (PG_ENCODING_IS_CLIENT_ONLY(encoding))
	{
		/*
		 * The byte-oriented lexer must not interpret an ASCII trail byte
		 * (e.g. in SJIS) as SQL punctuation. Keep offsets identical and
		 * use the original bytes when collecting literal contents.
		 */
		extra.wire_source = query;
		for (int i = 0; i < len; i++)
		{
			int			width;

			if (!IS_HIGHBIT_SET(query[i]))
				continue;
			width = pg_encoding_verifymbchar(encoding, query + i, len - i);
			for (int j = 1; j < width; j++)
			{
				i++;
				if (!IS_HIGHBIT_SET(query[i]))
					extra.core_yy_extra.scanbuf[i] = (char) 0x80;
			}
		}
	}
	initStringInfo(&out);
	for (;;)
	{
		int			token;

		extra.binary_token_end = 0;
		/* a NUL outside a string literal is token 0 as well */
		if ((token = mys_core_yylex(&value, &loc, scanner)) == 0)
			break;
		if (extra.binary_token_end > 0)
		{
			int n = atoi(value.str);

			appendBinaryStringInfo(&out, query + copied, loc - copied);
			/* The introducer may touch the quote: _binary'...' is valid. */
			appendStringInfoString(&out, prev_token == UNDERSCORE_BINARY ? " X'" : " _binary X'");
			if ((Size) n > (MaxAllocSize - 1) / 2)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("binary literal is too large")));
			enlargeStringInfo(&out, n * 2 + 1);
			out.len += hex_encode(value.str + 16, n, out.data + out.len);
			appendStringInfoChar(&out, '\'');
			copied = extra.binary_token_end;
		}
		prev_token = token;
	}
	mys_scanner_finish(scanner);
	appendBinaryStringInfo(&out, query + copied, len - copied);
	if (memchr(out.data, '\0', out.len))
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("NUL in query text outside a string literal")));
	return out.data;
}

List *
mysql_raw_parser(const char *str)
{
#ifdef HALO_MYSQL_GRAMMAR
	/* Syscache-dirty refresh: connections can span ALTER EXTENSION UPDATE. */
	mys_binary_session_init();
	return mys_raw_parser(str);
#else
	/* grammar not ported yet: fall back to the PostgreSQL grammar */
	return raw_parser(str, RAW_PARSE_DEFAULT);
#endif
}
