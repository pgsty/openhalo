/*-------------------------------------------------------------------------
 *
 * mys_compat.c
 *	  Functions that openHalo kept in its modified PostgreSQL core and that
 *	  the MySQL grammar depends on.
 *
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "nodes/value.h"
#include "utils/builtins.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

#include "halo_mysql.h"
#include "adapter/mysql/adapter.h"

/*
 * MySQL session state that openHalo kept in its modified backend
 * (adapter/mysql/adapter.c, tcop/postgres.c).
 */
char	   *halo_mysql_version = NULL;	/* GUC mysql.halo_mysql_version */
bool		halo_mysql_explicit_defaults_for_timestamp = true;
unsigned long foundRows = 0;
long long	affectedRows = 0;
unsigned long lastInsertID = 0;
bool		isStrictTransTablesOn = true;
Oid			caseInsensitiveId = InvalidOid;
bool		needCommitTrx = false;
bool		needStartNewTrx = false;

static bool coll_callback_registered = false;

static void
invalidate_case_insensitive_collation(Datum arg, int cacheid, uint32 hashvalue)
{
	caseInsensitiveId = InvalidOid;
}

/* OID of the case-insensitive collation used for MySQL *_ci collations */
Oid
getCaseInsensitiveId(void)
{
	if (!coll_callback_registered)
	{
		CacheRegisterSyscacheCallback(COLLOID,
									  invalidate_case_insensitive_collation,
									  (Datum) 0);
		coll_callback_registered = true;
	}
	if (!OidIsValid(caseInsensitiveId) &&
		IsTransactionState() && !IsAbortedTransactionBlockState())
		caseInsensitiveId = get_collation_oid(list_make2(makeString("mysql"),
														 makeString("case_insensitive")), true);
	return caseInsensitiveId;
}

extern Oid	getCurrentNamespaceOid(void);

/*
 * The schema MySQL considers the "current database".
 */
Oid
getCurrentNamespaceOid(void)
{
	if (!IsTransactionState() || IsAbortedTransactionBlockState())
		ereport(ERROR,
				(errcode(ERRCODE_IN_FAILED_SQL_TRANSACTION),
				 errmsg("current transaction is aborted, commands ignored until end of transaction block")));

	if (mys_is_mysql_session() && halo_mysql_parser == MYS_PARSER_MYSQL)
	{
		const char *db = mys_selected_database();
		Oid			nspid;

		if (db == NULL)
			ereport(ERROR,
					(errcode(MYS_ERRCODE_NO_DB_ERROR),
					 errmsg("No database selected")));
		nspid = get_namespace_oid(mys_catalog_schema_name(db), true);
		if (!OidIsValid(nspid))
			ereport(ERROR,
					(errcode(MYS_ERRCODE_NO_DB_ERROR),
					 errmsg("No database selected")));
		return nspid;
	}
	else
	{
		List	   *path = fetch_search_path(false);

		if (path == NIL)
			ereport(ERROR,
					(errcode(MYS_ERRCODE_NO_DB_ERROR),
					 errmsg("No database selected")));
		return linitial_oid(path);
	}
}

/*
 * mysql.database() / mysql.schema(): returns the currently selected MySQL
 * database (NULL when none is selected), or current_schema in PG sessions.
 */
PG_FUNCTION_INFO_V1(mys_database);
Datum
mys_database(PG_FUNCTION_ARGS)
{
	if (mys_is_mysql_session() && halo_mysql_parser == MYS_PARSER_MYSQL)
	{
		const char *db = mys_selected_database();

		if (db == NULL)
			PG_RETURN_NULL();
		PG_RETURN_TEXT_P(cstring_to_text(db));
	}
	else
	{
		List	   *path = fetch_search_path(false);
		char	   *nspname;

		if (path == NIL)
			PG_RETURN_NULL();
		nspname = get_namespace_name(linitial_oid(path));
		if (nspname == NULL)
			PG_RETURN_NULL();
		PG_RETURN_TEXT_P(cstring_to_text(nspname));
	}
}
