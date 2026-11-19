/*-------------------------------------------------------------------------
 *
 * systemVar.c
 *	  MySQL system variable SQL entry points
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"
#include "halo_mysql.h"

#include "adapter/mysql/systemVar.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"

/*
 * The wire/database representation is UTF-8. Keep initialization settings
 * honest about approximated encodings; literal collation propagation is a
 * separate compatibility boundary, not implemented by these setters.
 */
static const char *
connection_charset(const char *name)
{
	if (pg_strcasecmp(name, "utf8") == 0 || pg_strcasecmp(name, "utf8mb3") == 0)
		return "utf8";
	if (pg_strcasecmp(name, "utf8mb4") != 0)
		mys_add_warning(1105, psprintf("Character set '%s' mapped to utf8mb4", name));
	return "utf8mb4";
}

static void
set_connection_charset(FunctionCallInfo fcinfo, bool names)
{
	char	   *charset = text_to_cstring(PG_GETARG_TEXT_PP(0));
	const char *mapped_charset;
	const char *connection;
	char	   *collation = NULL;
	const char *effective;
	bool		binary;
	size_t		len;

	if (strcmp(charset, "halo_mysql_system_var_default") == 0)
	{
		pfree(charset);
		getSystemVariableValueForSelect("character_set_server", false, &charset);
		if (names)
			getSystemVariableValueForSelect("collation_server", false, &collation);
	}

	mapped_charset = connection_charset(charset);
	connection = mapped_charset;
	if (names)
	{
		if (PG_NARGS() > 1 && !PG_ARGISNULL(1))
			collation = text_to_cstring(PG_GETARG_TEXT_PP(1));
	}
	else
	{
		char	   *db_charset = NULL;

		getSystemVariableValueForSelect("character_set_database", true, &db_charset);
		connection = connection_charset(db_charset);
		pfree(db_charset);
		getSystemVariableValueForSelect("collation_database", true, &collation);
	}

	len = collation ? strlen(collation) : 0;
	/* Match the DDL mapper: *_ci uses general_ci, other names use bin. */
	binary = collation && !(len > 3 && pg_strcasecmp(collation + len - 3, "_ci") == 0);
	effective = strcmp(connection, "utf8") == 0 ?
		(binary ? "utf8_bin" : "utf8_general_ci") :
		(binary ? "utf8mb4_bin" : "utf8mb4_general_ci");
	if (collation && pg_strcasecmp(collation, effective) != 0)
		mys_add_warning(1105, psprintf("Collation '%s' mapped to %s", collation, effective));

	setSystemVariableValue("character_set_client", pstrdup(mapped_charset), true);
	setSystemVariableValue("character_set_results", pstrdup(mapped_charset), true);
	setSystemVariableValue("character_set_connection", pstrdup(connection), true);
	setSystemVariableValue("collation_connection", pstrdup(effective), true);

	pfree(charset);
	if (collation)
		pfree(collation);
}

PG_FUNCTION_INFO_V1(mys_set_names);
Datum
mys_set_names(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	set_connection_charset(fcinfo, true);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(mys_set_charset);
Datum
mys_set_charset(PG_FUNCTION_ARGS)
{
	if (PG_ARGISNULL(0))
		PG_RETURN_NULL();
	set_connection_charset(fcinfo, false);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(setSystemVariable);
Datum
setSystemVariable(PG_FUNCTION_ARGS)
{
	char	   *varName;
	char	   *varValue;
	bool		isSessionSystemVar;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(2))
		PG_RETURN_BOOL(false);

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));
	varValue = PG_ARGISNULL(1) ? NULL : TextDatumGetCString(PG_GETARG_DATUM(1));
	isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(2));

	setSystemVariableValue(varName, varValue, isSessionSystemVar);

	pfree(varName);
	if (varValue)
		pfree(varValue);
	PG_RETURN_BOOL(true);
}

PG_FUNCTION_INFO_V1(getSystemVariable);
Datum
getSystemVariable(PG_FUNCTION_ARGS)
{
	char	   *varName;
	bool		isSessionSystemVar;
	char	   *varValue = NULL;
	text	   *res;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));
	isSessionSystemVar = DatumGetBool(PG_GETARG_DATUM(1));
	getSystemVariableValueForSelect(varName, isSessionSystemVar, &varValue);
	pfree(varName);

	if (varValue == NULL)
		PG_RETURN_NULL();

	res = cstring_to_text(varValue);
	pfree(varValue);
	PG_RETURN_TEXT_P(res);
}

/*
 * SHOW VARIABLES exposes the same live settings as @@variables, with SHOW's
 * enum spellings (for example ON/OFF), rather than placeholder values.
 */
PG_FUNCTION_INFO_V1(mys_show_system_variable);
Datum
mys_show_system_variable(PG_FUNCTION_ARGS)
{
	char	   *name;
	char	   *value = NULL;
	text	   *res;

	if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
		PG_RETURN_NULL();

	name = text_to_cstring(PG_GETARG_TEXT_PP(0));
	if (!getSystemVariableValueForShow(name, PG_GETARG_BOOL(1), &value) ||
		value == NULL)
	{
		pfree(name);
		PG_RETURN_NULL();
	}

	pfree(name);
	res = cstring_to_text(value);
	pfree(value);
	PG_RETURN_TEXT_P(res);
}

/*
 * mysql.getenv(name): environment variable of the server process, restricted
 * to superusers and used at extension installation time for @@datadir and
 * @@character_sets_dir.
 */
PG_FUNCTION_INFO_V1(getENV);
Datum
getENV(PG_FUNCTION_ARGS)
{
	char	   *varName;
	const char *envValue;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to call mysql.getenv()")));

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("environment variable must not be null")));

	varName = TextDatumGetCString(PG_GETARG_DATUM(0));
	envValue = getenv(varName);
	if (envValue == NULL && strcmp(varName, "PGDATA") == 0 && DataDir != NULL)
		envValue = DataDir;
	if (envValue == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("The environment variable %s is not set yet.", varName)));

	pfree(varName);
	PG_RETURN_TEXT_P(cstring_to_text(envValue));
}
