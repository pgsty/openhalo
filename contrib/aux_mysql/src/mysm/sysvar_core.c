/*-------------------------------------------------------------------------
 *
 * sysvar_core.c
 *	  MySQL session and global system variable management
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

#include <time.h>

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "adapter/mysql/adapter.h"
#include "adapter/mysql/systemVar.h"
#include "catalog/namespace.h"
#include "commands/trigger.h"
#include "executor/tuptable.h"
#include "lib/stringinfo.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "openhalo_version.h"
#include "storage/ipc.h"
#include "storage/lockdefs.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/elog.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#define SYSTEM_VAR_NAME_MAX_LEN			128
#define SYSTEM_VAR_VALUE_MAX_LEN		512
#define SYSTEM_VAR_INT_NUM				512
#define SYSTEM_VAR_MAX_NUM				1024
#define SYSTEM_VAR_RESULT_RULE_NUM		16
#define SYSTEM_VAR_RESULT_RULE_LEN		64
#define SYSVAR_LWLOCK_TRANCHE			"aux_mysql_sysvar"

uint64		mys_sqlMode = MYS_MODE_ONLY_FULL_GROUP_BY |
	MYS_MODE_STRICT_TRANS_TABLES |
	MYS_MODE_NO_ZERO_IN_DATE |
	MYS_MODE_NO_ZERO_DATE |
	MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO |
	MYS_MODE_NO_AUTO_CREATE_USER |
	MYS_MODE_NO_ENGINE_SUBSTITUTION;
int			autoCommit = 2;
int			default_week_format = 0;

HTAB	   *globalSystemVars = NULL;
static LWLock *sysvar_lwlock = NULL;

typedef struct SystemVar
{
	char		varName[SYSTEM_VAR_NAME_MAX_LEN];	/* hash key must be first */
	char		varDefValue[SYSTEM_VAR_VALUE_MAX_LEN];
	char		varConfValue[SYSTEM_VAR_VALUE_MAX_LEN];
	int			sessionGlobalType;	/* 0:global & session, 1:global, 2:session, 3:sess-global */
	bool		sessionDefValueFromGlobal;
	bool		isReadWrite;
	int			resultNum;
	char		result[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
	int			selectResultNum;
	char		selectResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
	int			showResultNum;
	char		showResult[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN];
} SystemVar;

static SystemVar *baseSystemVars[SYSTEM_VAR_MAX_NUM];
static int	baseSystemVarsNum = 0;
static HTAB *sessionSystemVars = NULL;

static List *readBaseVariableRecord(TupleTableSlot *slot);
static List *readBaseVariablesTable(void);
static bool add2BaseSystemVars(SystemVar *systemVar);
static void add2GlobalSystemVarsLocked(SystemVar *systemVar);
static void add2SessionSystemVars(SystemVar *systemVar, bool at_init);
static SystemVar *initBaseSystemVar(List *systemVarColumnVals, MemoryContext targetCxt);
static void initSystemVariables(void);
static void initBaseSystemVars(void);
static void initGlobalSystemVarsLocked(SystemVar *systemVars[], int systemVarsNum);
static void initSessionSystemVarsLocked(SystemVar *systemVars[], int systemVarsNum);
static SystemVar *dynAddSystemVar(const char *varName);
static void lowerSystemVarName(char *varName);
static bool assignSystemVar(const char *varDefValue,
							const char *varConfValue,
							int sessionGlobalType,
							bool sessionDefValueFromGlobal,
							bool isReadWrite,
							const char *result,
							const char *selectResult,
							const char *showResult,
							SystemVar *systemVar);
static bool assignSystemVarResult(const char *srcResult,
								  int *resultNum,
								  char result[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN]);
static void copySystemVar(const SystemVar *srcSystemVar, SystemVar *destSystemVar);
static void setSystemVarValueByVarName(const char *varName,
									   const char *value,
									   char *varValue);
static void setSystemVarValueForOptimizerSwitch(const char *value,
												char *varValue);
static void setSystemVarValue(const char *value, char *varValue);
static void rectifySystemVarValue(const char *varName, char **varValue,
								  bool isSessionSystemVar);
static void getDefaultValue(const char *varName, bool isSessionSystemVar,
							char **varValue);
static void applySystemVarValue(const char *varName, const char *varValue);
static void setSystemVariableValueOne(const char *rawVarName, const char *rawVarValue,
									  bool isSessionSystemVar);
static int	getSystemVariableValueImpl(const char *rawVarName,
									   bool isSessionSystemVar,
									   int selectShowType,
									   char **varValue);
static char *getConfValue(const SystemVar *systemVar, int selectShowType);
static char *resolveMysqlTimeZoneForPG(const char *tzVal);

/* ----------------------------------------------------------------
 *	Shared-memory initialization (LWLock-protected, no spinlocks)
 * ----------------------------------------------------------------
 */

Size
GlobalSystemVariablesShmemSize(void)
{
	return hash_estimate_size(SYSTEM_VAR_MAX_NUM, sizeof(SystemVar));
}

void
GlobalSystemVariablesShmemRequest(void)
{
	RequestNamedLWLockTranche(SYSVAR_LWLOCK_TRANCHE, 1);
}

void
GlobalSystemVariablesShmemInit(void)
{
	HASHCTL		hashctl;

	sysvar_lwlock = &(GetNamedLWLockTranche(SYSVAR_LWLOCK_TRANCHE))->lock;

	memset(&hashctl, 0, sizeof(hashctl));
	hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
	hashctl.entrysize = sizeof(SystemVar);
	globalSystemVars = ShmemInitHash("MySQL Global System Variables",
									 SYSTEM_VAR_MAX_NUM,
									 &hashctl,
									 HASH_ELEM | HASH_STRINGS);
}

static inline bool
global_shmem_ready(void)
{
	return (globalSystemVars != NULL && sysvar_lwlock != NULL);
}

static bool
lookup_global_system_var(const char *varName, SystemVar *out_copy)
{
	SystemVar  *found_var;
	bool		found = false;

	if (!global_shmem_ready())
		return false;

	LWLockAcquire(sysvar_lwlock, LW_SHARED);
	found_var = (SystemVar *) hash_search(globalSystemVars, varName, HASH_FIND, &found);
	if (found && out_copy != NULL)
		*out_copy = *found_var;
	LWLockRelease(sysvar_lwlock);

	return found;
}

/* ----------------------------------------------------------------
 *	Special pseudo / GUC-backed MySQL system variables
 * ----------------------------------------------------------------
 */

static bool
getVersionVariable(const char *varName, char **varValue)
{
	if (pg_strcasecmp(varName, "version_comment") == 0)
	{
		*varValue = pstrdup("OpenHalo " OPENHALO_VERSION " (PostgreSQL " PG_VERSION ")");
		return true;
	}
	if (pg_strcasecmp(varName, "gtid_mode") == 0)
	{
		*varValue = pstrdup("OFF");
		return true;
	}
	if (pg_strcasecmp(varName, "gtid_executed") == 0 ||
		pg_strcasecmp(varName, "gtid_purged") == 0)
	{
		*varValue = pstrdup("");
		return true;
	}
	if (pg_strcasecmp(varName, "max_allowed_packet") == 0)
	{
		*varValue = psprintf("%d", halo_mysql_max_allowed_packet);
		return true;
	}
	if (halo_mysql_version != NULL && pg_strcasecmp(varName, "version") == 0)
	{
		*varValue = pstrdup(halo_mysql_version);
		return true;
	}
	return false;
}

void
getSystemVariableValueForSelect(char *varName,
								bool isSessionSystemVar,
								char **varValue)
{
	int			getRet;

	if (varName != NULL && getVersionVariable(varName, varValue))
		return;

	getRet = getSystemVariableValueImpl(varName,
										isSessionSystemVar,
										1,
										varValue);
	if (getRet == 0)
		return;
	else if (getRet == 1)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("Unknown system variable '%s'", varName ? varName : "")));
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable '%s' is a SESSION variable", varName)));
}

bool
getSystemVariableValueForShow(char *varName,
							  bool isSessionSystemVar,
							  char **varValue)
{
	int			getRet;

	if (varName != NULL && getVersionVariable(varName, varValue))
		return true;

	getRet = getSystemVariableValueImpl(varName,
										isSessionSystemVar,
										2,
										varValue);
	if (getRet == 0)
		return true;
	else if (getRet == 1)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("Unknown system variable '%s'", varName ? varName : "")));
	else
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable '%s' is a SESSION variable", varName)));
	return false;
}

/* ----------------------------------------------------------------
 *	MySQL transaction characteristics & read_only tracking
 * ----------------------------------------------------------------
 */

static char pending_isolation[32];
static int	pending_read_only = -1;
static bool pending_armed = false;
static bool txn_used = false;
static bool txn_explicit = false;
static bool txn_started = false;
static bool stmt_touched = false;
bool		mys_server_read_only = false;

static bool
is_isolation_var(const char *name)
{
	return strcmp(name, "transaction_isolation") == 0 ||
		strcmp(name, "tx_isolation") == 0;
}

static bool
is_tx_read_only_var(const char *name)
{
	return strcmp(name, "transaction_read_only") == 0 ||
		strcmp(name, "tx_read_only") == 0;
}

static const char *
canonical_isolation(const char *value)
{
	static const char *const levels[] = {
		"READ-UNCOMMITTED", "READ-COMMITTED", "REPEATABLE-READ", "SERIALIZABLE"
	};
	char		buf[32];
	int			i;

	if (value == NULL || strlen(value) >= sizeof(buf))
		return NULL;
	if (value[0] >= '0' && value[0] <= '3' && value[1] == '\0')
		return levels[value[0] - '0'];
	for (i = 0; value[i]; i++)
		buf[i] = (value[i] == ' ' || value[i] == '_') ?
			'-' : pg_toupper((unsigned char) value[i]);
	buf[i] = '\0';
	for (i = 0; i < lengthof(levels); i++)
		if (strcmp(buf, levels[i]) == 0)
			return levels[i];
	return NULL;
}

static const char *
canonical_flag(const char *value)
{
	if (value == NULL)
		return NULL;
	if (strcmp(value, "1") == 0 || pg_strcasecmp(value, "on") == 0 ||
		pg_strcasecmp(value, "true") == 0 || pg_strcasecmp(value, "t") == 0)
		return "1";
	if (strcmp(value, "0") == 0 || pg_strcasecmp(value, "off") == 0 ||
		pg_strcasecmp(value, "false") == 0 || pg_strcasecmp(value, "f") == 0)
		return "0";
	return NULL;
}

static void
session_value(const char *name, char *buf, size_t len)
{
	SystemVar  *var;
	bool		found;

	buf[0] = '\0';
	if (sessionSystemVars == NULL)
		return;
	var = (SystemVar *) hash_search(sessionSystemVars, name, HASH_FIND, &found);
	if (found)
		strlcpy(buf, var->varConfValue, len);
}

static void
global_value(const char *name, char *buf, size_t len)
{
	SystemVar	copy;

	buf[0] = '\0';
	if (lookup_global_system_var(name, &copy))
		strlcpy(buf, copy.varConfValue, len);
}

static bool
global_flag(const char *name)
{
	char		value[16];
	const char *cf;

	global_value(name, value, sizeof(value));
	cf = canonical_flag(value);
	return cf != NULL && strcmp(cf, "1") == 0;
}

static void
set_guc(const char *name, const char *value)
{
	(void) set_config_option(name, value, PGC_SUSET, PGC_S_SESSION,
							 GUC_ACTION_SET, true, 0, false);
}

static int
pg_isolation(const char *level, bool read_only)
{
	if (strcmp(level, "SERIALIZABLE") == 0)
		return RecoveryInProgress() ? XACT_REPEATABLE_READ : XACT_SERIALIZABLE;
	if (strcmp(level, "REPEATABLE-READ") == 0 && read_only)
		return XACT_REPEATABLE_READ;
	return XACT_READ_COMMITTED;
}

static const char *
pg_isolation_name(int level)
{
	return level == XACT_SERIALIZABLE ? "serializable" :
		level == XACT_REPEATABLE_READ ? "repeatable read" : "read committed";
}

static const char *
next_transaction(bool *read_only)
{
	static char level[32];
	char		session_level[32];
	char		ro[16];
	const char *canon_sess;
	const char *canon_ro;

	session_value("transaction_isolation", session_level, sizeof(session_level));
	session_value("transaction_read_only", ro, sizeof(ro));
	canon_sess = canonical_isolation(session_level);
	canon_ro = canonical_flag(ro);

	strlcpy(level, pending_isolation[0] ? pending_isolation :
			canon_sess ? canon_sess : "REPEATABLE-READ", sizeof(level));
	*read_only = pending_read_only >= 0 ? pending_read_only == 1 :
		(canon_ro != NULL && strcmp(canon_ro, "1") == 0);
	return level;
}

static bool
have_pending(void)
{
	return pending_isolation[0] != '\0' || pending_read_only >= 0;
}

static void
spend_one_shot(void)
{
	pending_isolation[0] = '\0';
	pending_read_only = -1;
	pending_armed = false;
}

static void
new_transaction(void)
{
	if (pending_armed && (txn_used || stmt_touched || txn_explicit))
		spend_one_shot();
	txn_used = stmt_touched = txn_explicit = txn_started = false;
	pending_armed = have_pending();
}

void
mys_statement_touched_tables(void)
{
	stmt_touched = true;
}

bool
mys_transaction_has_block(void)
{
	return txn_started && IsTransactionBlock();
}

bool
mys_transaction_active(void)
{
	return mys_transaction_has_block() && (txn_explicit || txn_used || stmt_touched);
}

static void
sync_transaction_defaults(void)
{
	bool		read_only;
	const char *level;
	int			pglevel;

	if (sessionSystemVars == NULL)
		return;
	level = next_transaction(&read_only);
	pglevel = pg_isolation(level, read_only);
	if (DefaultXactIsoLevel != pglevel)
		set_guc("default_transaction_isolation", pg_isolation_name(pglevel));
	if (DefaultXactReadOnly != read_only)
		set_guc("default_transaction_read_only", read_only ? "on" : "off");
}

void
mys_sync_session_state(void)
{
	if (sessionSystemVars == NULL)
		initSystemVariables();
	mys_sync_foreign_key_checks();
	sync_transaction_defaults();
}

void
mys_start_implicit_transaction(bool ddl)
{
	bool		read_only;
	const char *level;
	int			pglevel;

	if (IsTransactionBlock() || FirstSnapshotSet || IsSubTransaction())
		return;
	new_transaction();
	if (ddl)
		spend_one_shot();
	level = next_transaction(&read_only);
	pglevel = pg_isolation(level, read_only);
	if (XactIsoLevel != pglevel)
		set_guc("transaction_isolation", pg_isolation_name(pglevel));
	if (XactReadOnly != read_only && !RecoveryInProgress())
		set_guc("transaction_read_only", read_only ? "on" : "off");
}

const char *
mys_transaction_start(int explicit_read_only, bool *snapshot, bool *read_only,
					  bool explicit_begin)
{
	const char *level;

	new_transaction();
	txn_started = true;
	txn_explicit = explicit_begin;
	level = next_transaction(read_only);
	if (explicit_read_only >= 0)
		*read_only = explicit_read_only == 1;
	if (*snapshot && strcmp(level, "REPEATABLE-READ") != 0)
	{
		mys_add_warning(138, "InnoDB: WITH CONSISTENT SNAPSHOT was ignored because this phrase "
						"can only be used with REPEATABLE READ isolation level.");
		*snapshot = false;
	}
	return *snapshot ? "repeatable read" : pg_isolation_name(pg_isolation(level, *read_only));
}

void
mys_statement_done(bool end, bool next_ddl)
{
	if (stmt_touched)
		txn_used = true;
	stmt_touched = false;
	if (IsTransactionState())
		return;
	new_transaction();
	if (end || next_ddl)
		spend_one_shot();
	mys_sync_foreign_key_checks();
	sync_transaction_defaults();
}

bool
mys_server_read_only_applies(void)
{
	mys_server_read_only = global_flag("super_read_only") ||
		(global_flag("read_only") && !superuser());
	return mys_server_read_only;
}

static bool
foreign_key_checks_off(const char *value)
{
	return value != NULL &&
		(pg_strcasecmp(value, "0") == 0 || pg_strcasecmp(value, "off") == 0 ||
		 pg_strcasecmp(value, "false") == 0 || pg_strcasecmp(value, "f") == 0);
}

bool
mys_foreign_key_checks(void)
{
	SystemVar  *var;
	bool		found;

	if (sessionSystemVars == NULL)
		return true;
	var = (SystemVar *) hash_search(sessionSystemVars, "foreign_key_checks",
									HASH_FIND, &found);
	return !found || !foreign_key_checks_off(var->varConfValue);
}

void
mys_sync_foreign_key_checks(void)
{
	/* RI trigger and TRUNCATE hooks consult the session variable directly. */
}

/* ----------------------------------------------------------------
 *	SET [SESSION | GLOBAL] system variable implementation
 * ----------------------------------------------------------------
 */

static void
setSystemVariableValueOne(const char *rawVarName, const char *rawVarValue,
						  bool isSessionSystemVar)
{
	char		varName[SYSTEM_VAR_NAME_MAX_LEN];
	char	   *varValue;

	if (rawVarName == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Invalid system variable name(NULL)")));

	if (strlen(rawVarName) >= sizeof(varName))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("Unknown system variable '%s'", rawVarName)));

	strlcpy(varName, rawVarName, sizeof(varName));
	lowerSystemVarName(varName);

	/* SET GLOBAL requires SUPER / superuser privilege (MySQL 1227). */
	if (!isSessionSystemVar && !superuser())
		ereport(ERROR,
				(errcode(MYS_ERRCODE_SPECIFIC_ACCESS_DENIED),
				 errmsg("Access denied; you need (at least one of) the SUPER or SYSTEM_VARIABLES_ADMIN privilege(s) for this operation")));

	if (strcmp(varName, "max_allowed_packet") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				 errmsg("set mysql.max_allowed_packet in postgresql.conf and reload")));

	if (sessionSystemVars == NULL)
		initSystemVariables();

	varValue = rawVarValue ? pstrdup(rawVarValue) : NULL;

	if (strcmp(varName, "gtid_mode") == 0)
	{
		if (varValue && (pg_strcasecmp(varValue, "OFF") == 0 ||
						 strcmp(varValue, "0") == 0 ||
						 pg_strcasecmp(varValue, "DEFAULT") == 0))
			return;
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("MySQL GTID replication is unavailable; transactions have no MySQL GTID")));
	}
	if (strcmp(varName, "gtid_executed") == 0 || strcmp(varName, "gtid_purged") == 0 ||
		strcmp(varName, "version") == 0 || strcmp(varName, "version_comment") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
				 errmsg("Variable '%s' is read only", varName)));

	/* SET TRANSACTION without a scope: the next transaction only */
	if (strncmp(varName, "next_", 5) == 0 &&
		(is_isolation_var(varName + 5) || is_tx_read_only_var(varName + 5)))
	{
		const char *value = is_isolation_var(varName + 5) ?
			canonical_isolation(varValue) : canonical_flag(varValue);

		if (mys_transaction_active())
			ereport(ERROR,
					(errcode(MYS_ERRCODE_TXN_CHARACTERISTICS),
					 errmsg("Transaction characteristics can't be changed while a transaction is in progress")));
		if (value == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable '%s' can't be set to the value of '%s'",
							varName + 5, varValue ? varValue : "NULL")));
		if (is_isolation_var(varName + 5))
			strlcpy(pending_isolation, value, sizeof(pending_isolation));
		else
			pending_read_only = strcmp(value, "1") == 0;
		return;
	}

	/*
	 * In MySQL, character_set_results is the only system variable that may be
	 * set to NULL; every other system variable rejects NULL with 1231.
	 */
	if (varValue == NULL && strcmp(varName, "character_set_results") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable '%s' can't be set to the value of 'NULL'", varName)));

	rectifySystemVarValue(varName, &varValue, isSessionSystemVar);

	if (is_isolation_var(varName) || is_tx_read_only_var(varName))
	{
		const char *value = is_isolation_var(varName) ?
			canonical_isolation(varValue) : canonical_flag(varValue);

		if (value == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable '%s' can't be set to the value of '%s'",
							varName, varValue ? varValue : "NULL")));
		varValue = pstrdup(value);
	}

	if (isSessionSystemVar)
	{
		SystemVar  *systemVar;
		bool		found;
		char		candidate[SYSTEM_VAR_VALUE_MAX_LEN];

		systemVar = (SystemVar *) hash_search(sessionSystemVars,
											  varName,
											  HASH_FIND,
											  &found);
		if (!found && (systemVar = dynAddSystemVar(varName)) == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("Unknown system variable '%s'", varName)));

		if (systemVar->sessionGlobalType != 0 &&
			systemVar->sessionGlobalType != 2 &&
			systemVar->sessionGlobalType != 3)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable '%s' is a GLOBAL variable and should be set with SET GLOBAL",
							varName)));

		if (!systemVar->isReadWrite)
			ereport(ERROR,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("Variable '%s' is a read only variable", varName)));

		/*
		 * Validate and apply side effects against a stack copy BEFORE mutating
		 * TopMemoryContext state so a failed SET leaves @@var untouched.
		 */
		strlcpy(candidate, systemVar->varConfValue, sizeof(candidate));
		setSystemVarValueByVarName(varName, varValue, candidate);
		applySystemVarValue(varName, varValue ? candidate : NULL);
		strlcpy(systemVar->varConfValue, candidate, sizeof(systemVar->varConfValue));
	}
	else
	{
		SystemVar	globalCopy;
		bool		found;
		char		candidate[SYSTEM_VAR_VALUE_MAX_LEN];

		found = lookup_global_system_var(varName, &globalCopy);
		if (!found)
		{
			if (dynAddSystemVar(varName) == NULL ||
				!lookup_global_system_var(varName, &globalCopy))
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
						 errmsg("Unknown system variable '%s'", varName)));
		}

		if (globalCopy.sessionGlobalType != 0 &&
			globalCopy.sessionGlobalType != 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable '%s' is a SESSION variable and can't be used with SET GLOBAL",
							varName)));

		if (!globalCopy.isReadWrite)
			ereport(ERROR,
					(errcode(ERRCODE_CANT_CHANGE_RUNTIME_PARAM),
					 errmsg("Variable '%s' is a read only variable", varName)));

		strlcpy(candidate, globalCopy.varConfValue, sizeof(candidate));
		setSystemVarValueByVarName(varName, varValue, candidate);

		if (global_shmem_ready())
		{
			SystemVar  *gvar;
			bool		gfound;

			LWLockAcquire(sysvar_lwlock, LW_EXCLUSIVE);
			gvar = (SystemVar *) hash_search(globalSystemVars, varName, HASH_FIND, &gfound);
			if (gfound)
				strlcpy(gvar->varConfValue, candidate, sizeof(gvar->varConfValue));
			LWLockRelease(sysvar_lwlock);
		}

		if (globalCopy.sessionGlobalType == 1 && globalCopy.sessionDefValueFromGlobal)
		{
			SystemVar  *svar;
			bool		sfound;

			svar = (SystemVar *) hash_search(sessionSystemVars, varName, HASH_FIND, &sfound);
			if (sfound)
				strlcpy(svar->varConfValue, candidate, sizeof(svar->varConfValue));
		}
	}
}

void
setSystemVariableValue(char *varName, char *varValue, bool isSessionSystemVar)
{
	const char *also = NULL;
	char		normName[SYSTEM_VAR_NAME_MAX_LEN];
	char		value[SYSTEM_VAR_VALUE_MAX_LEN];

	setSystemVariableValueOne(varName, varValue, isSessionSystemVar);

	strlcpy(normName, varName, sizeof(normName));
	lowerSystemVarName(normName);

	if (strncmp(normName, "next_", 5) == 0)
		return;
	if (isSessionSystemVar && is_isolation_var(normName))
		pending_isolation[0] = '\0';
	else if (isSessionSystemVar && is_tx_read_only_var(normName))
		pending_read_only = -1;

	if (isSessionSystemVar)
		session_value(normName, value, sizeof(value));
	else
		global_value(normName, value, sizeof(value));

	if (strcmp(normName, "transaction_isolation") == 0)
		also = "tx_isolation";
	else if (strcmp(normName, "tx_isolation") == 0)
		also = "transaction_isolation";
	else if (strcmp(normName, "transaction_read_only") == 0)
		also = "tx_read_only";
	else if (strcmp(normName, "tx_read_only") == 0)
		also = "transaction_read_only";
	else if (strcmp(normName, "super_read_only") == 0 && strcmp(value, "1") == 0)
		also = "read_only";
	else if (strcmp(normName, "read_only") == 0 && strcmp(value, "0") == 0)
		also = "super_read_only";

	if (also)
		setSystemVariableValueOne(also, value, isSessionSystemVar);
}

void
setSystemVariableDatum(char *varName, Datum varConfValue,
					   Oid varValueType, bool isNull,
					   bool isSessionSystemVar)
{
	char	   *varValue = NULL;

	if (!isNull)
	{
		if (mys_is_mysql_session() && mys_binary_base_type(varValueType) == BYTEAOID)
		{
			bytea	   *b = DatumGetByteaPP(varConfValue);

			pg_verifymbstr(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b), false);
			varValue = pnstrdup(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
		}
		else
		{
			Oid			typoutput;
			bool		typIsVarlena;

			getTypeOutputInfo(varValueType, &typoutput, &typIsVarlena);
			varValue = OidOutputFunctionCall(typoutput, varConfValue);
		}
	}

	setSystemVariableValue(varName, varValue, isSessionSystemVar);
	if (varValue)
		pfree(varValue);
}

bool
isSystemVariable(char *varName)
{
	char		normName[SYSTEM_VAR_NAME_MAX_LEN];
	bool		found = false;

	if (varName == NULL || strlen(varName) >= sizeof(normName))
		return false;

	if (sessionSystemVars == NULL)
		initSystemVariables();

	strlcpy(normName, varName, sizeof(normName));
	lowerSystemVarName(normName);

	(void) hash_search(sessionSystemVars, normName, HASH_FIND, &found);
	return found;
}

/* ----------------------------------------------------------------
 *	Catalog loading and session / global initialization
 * ----------------------------------------------------------------
 */

static void
initSystemVariables(void)
{
	/* Load base_variables in a short-lived context, keeping only SystemVar structs in TopMemoryContext */
	initBaseSystemVars();

	if (global_shmem_ready())
	{
		LWLockAcquire(sysvar_lwlock, LW_EXCLUSIVE);
		initGlobalSystemVarsLocked(baseSystemVars, baseSystemVarsNum);
		LWLockRelease(sysvar_lwlock);

		LWLockAcquire(sysvar_lwlock, LW_SHARED);
		initSessionSystemVarsLocked(baseSystemVars, baseSystemVarsNum);
		LWLockRelease(sysvar_lwlock);
	}
	else
	{
		initSessionSystemVarsLocked(baseSystemVars, baseSystemVarsNum);
	}

	/* Apply outside any LWLock: applying a value may invoke GUC routines or allocate */
	for (int i = 0; i < baseSystemVarsNum; i++)
	{
		SystemVar  *var = (SystemVar *) hash_search(sessionSystemVars,
													baseSystemVars[i]->varName,
													HASH_FIND, NULL);

		if (var)
			applySystemVarValue(var->varName, var->varConfValue);
	}
}

static bool
add2BaseSystemVars(SystemVar *systemVar)
{
	if (baseSystemVarsNum < SYSTEM_VAR_MAX_NUM)
	{
		baseSystemVars[baseSystemVarsNum++] = systemVar;
		return true;
	}
	return false;
}

static SystemVar *
initBaseSystemVar(List *systemVarDatas, MemoryContext targetCxt)
{
	SystemVar  *systemVar;
	char	   *varName = NULL;
	char	   *varDefValue = NULL;
	char	   *varConfValue = NULL;
	int			sessionGlobalType = 0;
	bool		sessionDefValueFromGlobal = true;
	bool		isReadWrite = true;
	char	   *result = NULL;
	char	   *selectResult = NULL;
	char	   *showResult = NULL;
	ListCell   *lc;
	int			ind = 0;

	foreach(lc, systemVarDatas)
	{
		Node	   *nd = lfirst(lc);

		if (nd != NULL)
		{
			char	   *val = strVal(nd);

			if (ind == 0)
			{
				varName = val;
				lowerSystemVarName(varName);
			}
			else if (ind == 1)
				varDefValue = val;
			else if (ind == 2)
				varConfValue = val;
			else if (ind == 3)
			{
				if (val[0] >= '0' && val[0] <= '3')
					sessionGlobalType = val[0] - '0';
				else
					return NULL;
			}
			else if (ind == 4)
				sessionDefValueFromGlobal = (val[0] == '1' || val[0] == 't');
			else if (ind == 5)
				isReadWrite = (val[0] == '1' || val[0] == 't');
			else if (ind == 6)
				result = val;
			else if (ind == 7)
				selectResult = val;
			else
				showResult = val;
		}
		++ind;
	}

	if (varName == NULL)
		return NULL;

	systemVar = (SystemVar *) MemoryContextAllocZero(targetCxt, sizeof(SystemVar));
	strlcpy(systemVar->varName, varName, sizeof(systemVar->varName));
	if (!assignSystemVar(varDefValue,
						 varConfValue,
						 sessionGlobalType,
						 sessionDefValueFromGlobal,
						 isReadWrite,
						 result,
						 selectResult,
						 showResult,
						 systemVar))
	{
		pfree(systemVar);
		return NULL;
	}

	return systemVar;
}

static List *
readBaseVariableRecord(TupleTableSlot *slot)
{
	List	   *columnVals = NIL;
	TupleDesc	tupleDesc = slot->tts_tupleDescriptor;
	int			columnCount = tupleDesc->natts;

	for (int i = 0; i < columnCount; i++)
	{
		Form_pg_attribute att = TupleDescAttr(tupleDesc, i);
		bool		is_null;
		Datum		attr = slot_getattr(slot, i + 1, &is_null);

		if (!is_null)
		{
			Oid			typoutput;
			bool		typisvarlena;
			char	   *columnVal;

			getTypeOutputInfo(att->atttypid, &typoutput, &typisvarlena);
			columnVal = OidOutputFunctionCall(typoutput, attr);
			columnVals = lappend(columnVals, makeString(columnVal));
		}
		else
		{
			columnVals = lappend(columnVals, NULL);
		}
	}

	return columnVals;
}

static List *
readBaseVariablesTable(void)
{
	List	   *systemVarsDatas = NIL;
	Oid			schemaId;
	Oid			tableId;
	Relation	relation;
	Snapshot	snapshot;
	TableScanDesc scan;
	TupleTableSlot *slot;

	schemaId = get_namespace_oid("mys_informa_schema", true);
	if (!OidIsValid(schemaId))
		return NIL;
	tableId = get_relname_relid("base_variables", schemaId);
	if (!OidIsValid(tableId))
		return NIL;

	relation = table_open(tableId, AccessShareLock);
	snapshot = RegisterSnapshot(GetLatestSnapshot());
	scan = table_beginscan(relation, snapshot, 0, NULL, 0);
	slot = table_slot_create(relation, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		List	   *systemVarDatas = readBaseVariableRecord(slot);

		systemVarsDatas = lappend(systemVarsDatas, systemVarDatas);
	}
	table_endscan(scan);
	UnregisterSnapshot(snapshot);
	ExecDropSingleTupleTableSlot(slot);
	table_close(relation, AccessShareLock);

	return systemVarsDatas;
}

static void
initBaseSystemVars(void)
{
	MemoryContext scanCxt;
	MemoryContext oldCxt;
	List	   *systemVarsColumnVals;
	ListCell   *lc;

	for (int i = 0; i < baseSystemVarsNum; i++)
	{
		if (baseSystemVars[i] != NULL)
		{
			pfree(baseSystemVars[i]);
			baseSystemVars[i] = NULL;
		}
	}
	baseSystemVarsNum = 0;

	scanCxt = AllocSetContextCreate(CurrentMemoryContext,
									"aux_mysql base_variables scan",
									ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(scanCxt);

	systemVarsColumnVals = readBaseVariablesTable();
	foreach(lc, systemVarsColumnVals)
	{
		List	   *systemVarColumnVals = (List *) lfirst(lc);
		SystemVar  *systemVar = initBaseSystemVar(systemVarColumnVals, TopMemoryContext);

		if (systemVar != NULL)
		{
			if (!add2BaseSystemVars(systemVar))
			{
				pfree(systemVar);
				MemoryContextSwitchTo(oldCxt);
				MemoryContextDelete(scanCxt);
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("There are too many MySQL system variables.")));
			}
		}
		else
		{
			const char *vname = (systemVarColumnVals != NIL && linitial(systemVarColumnVals) != NULL) ?
				strVal(linitial(systemVarColumnVals)) : "<null>";
			char	   *vnameCopy = MemoryContextStrdup(oldCxt, vname);

			MemoryContextSwitchTo(oldCxt);
			MemoryContextDelete(scanCxt);
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("Invalid data '%s' in mys_informa_schema.base_variables table.",
							vnameCopy)));
		}
	}

	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(scanCxt);
}

static void
add2GlobalSystemVarsLocked(SystemVar *systemVar)
{
	SystemVar  *globalSystemVar;
	bool		found;

	if (!global_shmem_ready())
		return;

	globalSystemVar = (SystemVar *) hash_search(globalSystemVars,
												systemVar->varName,
												HASH_ENTER,
												&found);
	if (!found)
		copySystemVar(systemVar, globalSystemVar);
}

static void
initGlobalSystemVarsLocked(SystemVar *systemVars[], int systemVarsNum)
{
	SystemVar  *globalSystemVar;
	bool		found;

	if (!global_shmem_ready())
		return;

	globalSystemVar = (SystemVar *) hash_search(globalSystemVars,
												"global_system_vars_init_flag",
												HASH_ENTER,
												&found);
	if (!found)
	{
		(void) assignSystemVar("1", "1", 0, false, false, NULL, NULL, NULL,
							   globalSystemVar);
		for (int i = 0; i < systemVarsNum; i++)
			add2GlobalSystemVarsLocked(systemVars[i]);
	}
}

static void
add2SessionSystemVars(SystemVar *systemVar, bool at_init)
{
	SystemVar  *sessionSystemVar;
	bool		found;

	sessionSystemVar = (SystemVar *) hash_search(sessionSystemVars,
												 systemVar->varName,
												 HASH_ENTER,
												 &found);
	if (!found)
	{
		copySystemVar(systemVar, sessionSystemVar);
		if (at_init && sessionSystemVar->sessionDefValueFromGlobal && global_shmem_ready())
		{
			SystemVar  *global = (SystemVar *) hash_search(globalSystemVars,
														   sessionSystemVar->varName,
														   HASH_FIND, NULL);

			if (global)
				strlcpy(sessionSystemVar->varConfValue, global->varConfValue,
						sizeof(sessionSystemVar->varConfValue));
		}
	}

	if (!at_init)
		applySystemVarValue(sessionSystemVar->varName,
							sessionSystemVar->varConfValue);
}

static void
initSessionSystemVarsLocked(SystemVar *systemVars[], int systemVarsNum)
{
	HASHCTL		hashctl;

	memset(&hashctl, 0, sizeof(hashctl));
	hashctl.keysize = SYSTEM_VAR_NAME_MAX_LEN;
	hashctl.entrysize = sizeof(SystemVar);
	hashctl.hcxt = TopMemoryContext;
	sessionSystemVars = hash_create("Halo MySQL Session System Variables",
									SYSTEM_VAR_MAX_NUM,
									&hashctl,
									HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

	for (int i = 0; i < systemVarsNum; i++)
		add2SessionSystemVars(systemVars[i], true);
}

void
resetSessionSystemVariables(void)
{
	HASH_SEQ_STATUS scan;
	SystemVar  *entry;

	pending_isolation[0] = '\0';
	pending_read_only = -1;
	pending_armed = txn_used = txn_explicit = txn_started = stmt_touched = false;
	mys_server_read_only = false;

	if (sessionSystemVars == NULL)
		initSystemVariables();

	hash_seq_init(&scan, sessionSystemVars);
	while ((entry = hash_seq_search(&scan)) != NULL)
	{
		SystemVar	globalCopy;

		if (entry->sessionDefValueFromGlobal &&
			lookup_global_system_var(entry->varName, &globalCopy))
			strlcpy(entry->varConfValue, globalCopy.varConfValue,
					sizeof(entry->varConfValue));
		else
			strlcpy(entry->varConfValue, entry->varDefValue,
					sizeof(entry->varConfValue));

		applySystemVarValue(entry->varName, entry->varConfValue);
	}
	needCommitTrx = needStartNewTrx = false;
}

static SystemVar *
dynAddSystemVar(const char *varName)
{
	MemoryContext scanCxt;
	MemoryContext oldCxt;
	List	   *systemVarsColumnVals;
	ListCell   *lc;
	List	   *matchedRow = NIL;
	SystemVar  *systemVar = NULL;
	SystemVar  *sessionEntry = NULL;

	if (!IsTransactionState())
		return NULL;

	scanCxt = AllocSetContextCreate(CurrentMemoryContext,
									"aux_mysql dynAddSystemVar scan",
									ALLOCSET_DEFAULT_SIZES);
	oldCxt = MemoryContextSwitchTo(scanCxt);

	systemVarsColumnVals = readBaseVariablesTable();
	foreach(lc, systemVarsColumnVals)
	{
		List	   *curRow = (List *) lfirst(lc);

		if (curRow != NIL && linitial(curRow) != NULL)
		{
			const char *variableName = strVal(linitial(curRow));

			if (pg_strcasecmp(varName, variableName) == 0)
			{
				matchedRow = curRow;
				break;
			}
		}
	}

	if (matchedRow != NIL)
		systemVar = initBaseSystemVar(matchedRow, TopMemoryContext);

	MemoryContextSwitchTo(oldCxt);
	MemoryContextDelete(scanCxt);

	if (systemVar == NULL)
		return NULL;

	if (!add2BaseSystemVars(systemVar))
	{
		pfree(systemVar);
		return NULL;
	}

	if (global_shmem_ready())
	{
		LWLockAcquire(sysvar_lwlock, LW_EXCLUSIVE);
		add2GlobalSystemVarsLocked(systemVar);
		LWLockRelease(sysvar_lwlock);
	}

	add2SessionSystemVars(systemVar, false);
	sessionEntry = (SystemVar *) hash_search(sessionSystemVars,
											 systemVar->varName,
											 HASH_FIND, NULL);
	return sessionEntry;
}

/* ----------------------------------------------------------------
 *	Formatting, validation, and side-effect helpers
 * ----------------------------------------------------------------
 */

static void
lowerSystemVarName(char *varName)
{
	if (varName == NULL)
		return;
	for (size_t i = 0; varName[i] != '\0'; i++)
		varName[i] = pg_ascii_tolower((unsigned char) varName[i]);
}

static bool
assignSystemVar(const char *varDefValue,
				const char *varConfValue,
				int sessionGlobalType,
				bool sessionDefValueFromGlobal,
				bool isReadWrite,
				const char *result,
				const char *selectResult,
				const char *showResult,
				SystemVar *systemVar)
{
	setSystemVarValue(varDefValue, systemVar->varDefValue);
	setSystemVarValue(varConfValue, systemVar->varConfValue);
	systemVar->sessionGlobalType = sessionGlobalType;
	systemVar->sessionDefValueFromGlobal = sessionDefValueFromGlobal;
	systemVar->isReadWrite = isReadWrite;

	if (result != NULL)
	{
		if (!assignSystemVarResult(result, &systemVar->resultNum, systemVar->result))
			return false;
	}
	else
		systemVar->resultNum = 0;

	if (selectResult != NULL)
	{
		if (!assignSystemVarResult(selectResult, &systemVar->selectResultNum,
								   systemVar->selectResult))
			return false;
	}
	else
		systemVar->selectResultNum = 0;

	if (showResult != NULL)
	{
		if (!assignSystemVarResult(showResult, &systemVar->showResultNum,
								   systemVar->showResult))
			return false;
	}
	else
		systemVar->showResultNum = 0;

	if (systemVar->resultNum == 0 &&
		(systemVar->selectResultNum > 0 || systemVar->showResultNum > 0))
		return false;

	if (systemVar->resultNum > 0 &&
		((systemVar->selectResultNum != systemVar->resultNum &&
		  systemVar->showResultNum != systemVar->resultNum) ||
		 (systemVar->selectResultNum > 0 &&
		  systemVar->showResultNum > 0 &&
		  systemVar->selectResultNum != systemVar->showResultNum)))
		return false;

	return true;
}

static bool
assignSystemVarResult(const char *srcResult,
					  int *resultNum,
					  char result[SYSTEM_VAR_RESULT_RULE_NUM][SYSTEM_VAR_RESULT_RULE_LEN])
{
	int			count = 0;
	const char *p = srcResult;

	*resultNum = 0;
	if (srcResult == NULL || srcResult[0] == '\0')
		return true;

	while (*p != '\0')
	{
		const char *sep;
		size_t		tokLen;

		while (*p == '|')
			p++;
		if (*p == '\0')
			break;

		sep = strchr(p, '|');
		tokLen = sep ? (size_t) (sep - p) : strlen(p);

		if (count >= SYSTEM_VAR_RESULT_RULE_NUM ||
			tokLen >= SYSTEM_VAR_RESULT_RULE_LEN)
			return false;

		memcpy(result[count], p, tokLen);
		result[count][tokLen] = '\0';
		count++;

		if (!sep)
			break;
		p = sep + 1;
	}

	*resultNum = count;
	return true;
}

static void
copySystemVar(const SystemVar *srcSystemVar, SystemVar *destSystemVar)
{
	char		savedKey[SYSTEM_VAR_NAME_MAX_LEN];

	strlcpy(savedKey, destSystemVar->varName, sizeof(savedKey));
	*destSystemVar = *srcSystemVar;
	strlcpy(destSystemVar->varName, savedKey, sizeof(destSystemVar->varName));
}

static void
setSystemVarValueByVarName(const char *varName,
						   const char *value,
						   char *varValue)
{
	if (strcmp(varName, "optimizer_switch") == 0)
		setSystemVarValueForOptimizerSwitch(value, varValue);
	else
		setSystemVarValue(value, varValue);
}

static void
setSystemVarValueForOptimizerSwitch(const char *value, char *varValue)
{
	StringInfoData curBuf;
	const char *p;

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable 'optimizer_switch' can't be set to the value of 'NULL'")));

	if (pg_strcasecmp(value, "default") == 0)
		return;

	initStringInfo(&curBuf);
	appendStringInfoString(&curBuf, varValue);

	p = value;
	while (*p != '\0')
	{
		const char *eq = strchr(p, '=');
		const char *comma;
		size_t		nameLen;
		size_t		valLen;
		const char *valStart;
		char		canonVal[16];
		bool		matched = false;
		size_t		k = 0;

		if (eq == NULL || eq == p)
		{
			pfree(curBuf.data);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable 'optimizer_switch' can't be set to the value of '%s'", value)));
		}

		nameLen = (size_t) (eq - p);
		valStart = eq + 1;
		comma = strchr(valStart, ',');
		valLen = comma ? (size_t) (comma - valStart) : strlen(valStart);

		if (valLen == 2 && pg_strncasecmp(valStart, "on", 2) == 0)
			strlcpy(canonVal, "on", sizeof(canonVal));
		else if (valLen == 3 && pg_strncasecmp(valStart, "off", 3) == 0)
			strlcpy(canonVal, "off", sizeof(canonVal));
		else if (valLen == 7 && pg_strncasecmp(valStart, "default", 7) == 0)
			strlcpy(canonVal, "on", sizeof(canonVal));
		else
		{
			pfree(curBuf.data);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable 'optimizer_switch' can't be set to the value of '%s'", value)));
		}

		while (k < (size_t) curBuf.len)
		{
			const char *entryEnd = strchr(curBuf.data + k, ',');
			size_t		entryLen = entryEnd ? (size_t) (entryEnd - (curBuf.data + k)) : (size_t) curBuf.len - k;

			if (entryLen > nameLen &&
				curBuf.data[k + nameLen] == '=' &&
				pg_strncasecmp(curBuf.data + k, p, nameLen) == 0)
			{
				StringInfoData nextBuf;

				initStringInfo(&nextBuf);
				appendBinaryStringInfo(&nextBuf, curBuf.data, (int) k);
				appendBinaryStringInfo(&nextBuf, curBuf.data + k, (int) nameLen);
				appendStringInfo(&nextBuf, "=%s", canonVal);
				if (entryEnd != NULL)
					appendStringInfoString(&nextBuf, entryEnd);

				pfree(curBuf.data);
				curBuf = nextBuf;
				matched = true;
				break;
			}

			if (entryEnd == NULL)
				break;
			k = (size_t) (entryEnd - curBuf.data) + 1;
		}

		if (!matched)
		{
			pfree(curBuf.data);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable 'optimizer_switch' can't be set to the value of '%s'", value)));
		}

		if (comma == NULL)
			break;
		p = comma + 1;
		if (*p == '\0')
		{
			pfree(curBuf.data);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable 'optimizer_switch' can't be set to the value of '%s'", value)));
		}
	}

	if (curBuf.len >= SYSTEM_VAR_VALUE_MAX_LEN)
	{
		pfree(curBuf.data);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("optimizer_switch value exceeds maximum length")));
	}

	strlcpy(varValue, curBuf.data, SYSTEM_VAR_VALUE_MAX_LEN);
	pfree(curBuf.data);
}

static void
setSystemVarValue(const char *value, char *varValue)
{
	if (value != NULL)
		strlcpy(varValue, value, SYSTEM_VAR_VALUE_MAX_LEN);
	else
		varValue[0] = '\0';
}

static void
rectifySystemVarValue(const char *varName, char **varValue,
					  bool isSessionSystemVar)
{
	SystemVar  *systemVar;
	bool		found;
	int			varValueLen;

	if (*varValue == NULL)
		return;

	if (strcmp(*varValue, "halo_mysql_system_var_default") == 0)
	{
		getDefaultValue(varName, isSessionSystemVar, varValue);
		return;
	}

	if (strcmp(varName, "query_cache_size") == 0)
	{
		ErrorSaveContext escontext = {T_ErrorSaveContext};
		int64		val = pg_strtoint64_safe(*varValue, (Node *) &escontext);

		if (escontext.error_occurred || val < 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Incorrect argument type to variable 'query_cache_size'")));

		val = (val / 1048576) * 1048576;
		*varValue = psprintf(INT64_FORMAT, val);
		return;
	}

	varValueLen = (int) strlen(*varValue);
	systemVar = (SystemVar *) hash_search(sessionSystemVars,
										  varName,
										  HASH_FIND,
										  &found);
	if (!found && (systemVar = dynAddSystemVar(varName)) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("Unknown system variable '%s'", varName)));

	if (systemVar->resultNum > 0)
	{
		if (pg_strcasecmp(*varValue, "true") == 0 ||
			pg_strcasecmp(*varValue, "false") == 0)
		{
			*varValue = pstrdup(pg_strcasecmp(*varValue, "true") == 0 ? "1" : "0");
			varValueLen = 1;
		}

		for (int i = 0; i < systemVar->resultNum; i++)
		{
			int			resultItemLen = (int) strlen(systemVar->result[i]);

			if (varValueLen == resultItemLen &&
				pg_strncasecmp(*varValue, systemVar->result[i], varValueLen) == 0)
				return;

			if (i < systemVar->showResultNum)
			{
				int			showResultItemLen = (int) strlen(systemVar->showResult[i]);

				if (varValueLen == showResultItemLen &&
					pg_strncasecmp(*varValue, systemVar->showResult[i], varValueLen) == 0)
				{
					*varValue = pstrdup(systemVar->result[i]);
					return;
				}
			}
		}

		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable '%s' can't be set to the value of '%s'",
						varName, *varValue)));
	}
}

static void
getDefaultValue(const char *varName, bool isSessionSystemVar,
				char **varValue)
{
	bool		sessionDefValueFromGlobal = false;

	if (strcmp(varName, "last_insert_id") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Variable '%s' doesn't have a default value", varName)));

	if (isSessionSystemVar)
	{
		SystemVar  *systemVar;
		bool		found;

		systemVar = (SystemVar *) hash_search(sessionSystemVars,
											  varName,
											  HASH_FIND,
											  &found);
		if (!found)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("Unknown system variable '%s'", varName)));
		sessionDefValueFromGlobal = systemVar->sessionDefValueFromGlobal;
	}

	if (isSessionSystemVar && sessionDefValueFromGlobal)
	{
		SystemVar	globalCopy;

		if (lookup_global_system_var(varName, &globalCopy))
		{
			*varValue = pstrdup(globalCopy.varConfValue);
			return;
		}
	}

	for (int i = 0; i < baseSystemVarsNum; i++)
	{
		if (strcmp(varName, baseSystemVars[i]->varName) == 0)
		{
			*varValue = pstrdup(baseSystemVars[i]->varDefValue);
			return;
		}
	}

	ereport(ERROR,
			(errcode(ERRCODE_UNDEFINED_OBJECT),
			 errmsg("Unknown system variable '%s'", varName)));
}

/*
 * Convert a MySQL time_zone string ('SYSTEM', '+05:30', '-00:30', 'UTC',
 * 'Asia/Shanghai', ...) into a PostgreSQL TimeZone GUC string without
 * truncating half-hour offsets or flipping sign on '-00:30'.
 */
static char *
resolveMysqlTimeZoneForPG(const char *tzVal)
{
	if (tzVal == NULL || tzVal[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Unknown or incorrect time zone: '%s'", tzVal ? tzVal : "")));

	if (pg_strcasecmp(tzVal, "SYSTEM") == 0)
	{
		const char *logTz = GetConfigOption("log_timezone", true, false);
		time_t		now;
		struct tm	ltm;
		long		gmtoff;
		char		sign;

		if (logTz != NULL && logTz[0] != '\0' && pg_tzset(logTz) != NULL)
			return pstrdup(logTz);

		now = time(NULL);
		localtime_r(&now, &ltm);
		gmtoff = ltm.tm_gmtoff;
		sign = (gmtoff >= 0) ? '+' : '-';
		if (gmtoff < 0)
			gmtoff = -gmtoff;
		return psprintf("INTERVAL '%c%02ld:%02ld'",
						sign, gmtoff / 3600, (gmtoff % 3600) / 60);
	}

	if (tzVal[0] == '+' || tzVal[0] == '-')
	{
		char		sign = tzVal[0];
		const char *p = tzVal + 1;
		int			hh = 0;
		int			mm = 0;
		int			hdigits = 0;
		int			total_mins;

		while (isdigit((unsigned char) *p) && hdigits < 3)
		{
			hh = hh * 10 + (*p - '0');
			p++;
			hdigits++;
		}
		if (hdigits < 1 || hdigits > 2 || *p != ':')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Unknown or incorrect time zone: '%s'", tzVal)));
		p++;
		if (!isdigit((unsigned char) p[0]) || !isdigit((unsigned char) p[1]) || p[2] != '\0')
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Unknown or incorrect time zone: '%s'", tzVal)));
		mm = (p[0] - '0') * 10 + (p[1] - '0');
		if (mm > 59)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Unknown or incorrect time zone: '%s'", tzVal)));

		total_mins = hh * 60 + mm;
		if ((sign == '+' && total_mins > 14 * 60) ||
			(sign == '-' && total_mins > 13 * 60 + 59))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Unknown or incorrect time zone: '%s'", tzVal)));

		return psprintf("INTERVAL '%c%02d:%02d'", sign, hh, mm);
	}

	if (pg_tzset(tzVal) == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("Unknown or incorrect time zone: '%s'", tzVal)));

	return pstrdup(tzVal);
}

static void
applySystemVarValue(const char *varName, const char *varValue)
{
	if (varValue == NULL)
		return;

	if (strcmp(varName, "autocommit") == 0)
	{
		if (pg_strcasecmp(varValue, "1") == 0 ||
			pg_strcasecmp(varValue, "on") == 0)
		{
			if (autoCommit == 0)
			{
				needCommitTrx = true;
				needStartNewTrx = false;
			}
			else
			{
				needCommitTrx = false;
				needStartNewTrx = false;
			}
			autoCommit = 2;
		}
		else
		{
			if (autoCommit == 0)
			{
				needCommitTrx = false;
				needStartNewTrx = false;
			}
			else
			{
				needCommitTrx = false;
				needStartNewTrx = true;
			}
			autoCommit = 0;
		}
	}
	else if (strcmp(varName, "foreign_key_checks") == 0)
		mys_sync_foreign_key_checks();
	else if (is_isolation_var(varName) || is_tx_read_only_var(varName))
		sync_transaction_defaults();
	else if (strcmp(varName, "time_zone") == 0)
	{
		char	   *pgTz = resolveMysqlTimeZoneForPG(varValue);

		(void) set_config_option("timezone", pgTz, PGC_USERSET, PGC_S_SESSION,
								 GUC_ACTION_SET, true, ERROR, false);
		pfree(pgTz);
	}
	else if (strcmp(varName, "sql_mode") == 0)
	{
		char	   *rawValue = pstrdup(varValue);
		List	   *valueList = NIL;
		uint64		tempSqlMode = 0;

		static const struct
		{
			const char *name;
			uint64		bits;
		}			modes[] = {
			{"REAL_AS_FLOAT", MYS_MODE_REAL_AS_FLOAT},
			{"PIPES_AS_CONCAT", MYS_MODE_PIPES_AS_CONCAT},
			{"ANSI_QUOTES", MYS_MODE_ANSI_QUOTES},
			{"IGNORE_SPACE", MYS_MODE_IGNORE_SPACE},
			{"ONLY_FULL_GROUP_BY", MYS_MODE_ONLY_FULL_GROUP_BY},
			{"NO_UNSIGNED_SUBTRACTION", MYS_MODE_NO_UNSIGNED_SUBTRACTION},
			{"NO_DIR_IN_CREATE", MYS_MODE_NO_DIR_IN_CREATE},
			{"POSTGRESQL", MYS_MODE_POSTGRESQL},
			{"ORACLE", MYS_MODE_ORACLE},
			{"MSSQL", MYS_MODE_MSSQL},
			{"DB2", MYS_MODE_DB2},
			{"MAXDB", MYS_MODE_MAXDB},
			{"NO_KEY_OPTIONS", MYS_MODE_NO_KEY_OPTIONS},
			{"NO_TABLE_OPTIONS", MYS_MODE_NO_TABLE_OPTIONS},
			{"NO_FIELD_OPTIONS", MYS_MODE_NO_FIELD_OPTIONS},
			{"MYSQL323", MYS_MODE_MYSQL323},
			{"MYSQL40", MYS_MODE_MYSQL40},
			{"ANSI", MYS_MODE_ANSI | MYS_MODE_REAL_AS_FLOAT | MYS_MODE_PIPES_AS_CONCAT |
				MYS_MODE_ANSI_QUOTES | MYS_MODE_IGNORE_SPACE | MYS_MODE_ONLY_FULL_GROUP_BY},
			{"NO_AUTO_VALUE_ON_ZERO", MYS_MODE_NO_AUTO_VALUE_ON_ZERO},
			{"NO_BACKSLASH_ESCAPES", MYS_MODE_NO_BACKSLASH_ESCAPES},
			{"STRICT_TRANS_TABLES", MYS_MODE_STRICT_TRANS_TABLES},
			{"STRICT_ALL_TABLES", MYS_MODE_STRICT_ALL_TABLES},
			{"NO_ZERO_IN_DATE", MYS_MODE_NO_ZERO_IN_DATE},
			{"NO_ZERO_DATE", MYS_MODE_NO_ZERO_DATE},
			{"ALLOW_INVALID_DATES", MYS_MODE_INVALID_DATES},
			{"ERROR_FOR_DIVISION_BY_ZERO", MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO},
			{"TRADITIONAL", MYS_MODE_TRADITIONAL | MYS_MODE_STRICT_TRANS_TABLES |
				MYS_MODE_STRICT_ALL_TABLES | MYS_MODE_NO_ZERO_IN_DATE |
				MYS_MODE_NO_ZERO_DATE | MYS_MODE_ERROR_FOR_DIVISION_BY_ZERO |
				MYS_MODE_NO_ENGINE_SUBSTITUTION},
			{"NO_AUTO_CREATE_USER", MYS_MODE_NO_AUTO_CREATE_USER},
			{"HIGH_NOT_PRECEDENCE", MYS_MODE_HIGH_NOT_PRECEDENCE},
			{"NO_ENGINE_SUBSTITUTION", MYS_MODE_NO_ENGINE_SUBSTITUTION},
			{"PAD_CHAR_TO_FULL_LENGTH", MYS_MODE_PAD_CHAR_TO_FULL_LENGTH},
			{"TIME_TRUNCATE_FRACTIONAL", 0},
			{NULL, 0}
		};

		if (!SplitIdentifierString(rawValue, ',', &valueList))
		{
			pfree(rawValue);
			list_free(valueList);
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("Variable 'sql_mode' can't be set to the value of '%s'", varValue)));
		}

		foreach_ptr(char, curValue, valueList)
		{
			bool		known = false;

			if (strcmp(curValue, "0") == 0)
				continue;

			for (int m = 0; modes[m].name; m++)
			{
				if (pg_strcasecmp(curValue, modes[m].name) == 0)
				{
					tempSqlMode |= modes[m].bits;
					known = true;
					break;
				}
			}
			if (!known)
			{
				pfree(rawValue);
				list_free(valueList);
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("Variable 'sql_mode' can't be set to the value of '%s'", varValue)));
			}
		}

		pfree(rawValue);
		list_free(valueList);

		isStrictTransTablesOn = (tempSqlMode & (MYS_MODE_STRICT_TRANS_TABLES |
												MYS_MODE_STRICT_ALL_TABLES)) != 0;
		mys_sqlMode = tempSqlMode;
	}
	else if (strcmp(varName, "default_week_format") == 0)
	{
		char	   *endPtr;
		long		value;

		errno = 0;
		value = strtol(varValue, &endPtr, 10);
		if (errno == ERANGE || endPtr == varValue || *endPtr != '\0' ||
			value < 0 || value > 7)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("incorrect default_week_format value: %s", varValue)));

		default_week_format = (int) value;
	}
}

/* ----------------------------------------------------------------
 *	SELECT / SHOW variable lookup
 * ----------------------------------------------------------------
 */

static int
getSystemVariableValueImpl(const char *rawVarName,
						   bool isSessionSystemVar,
						   int selectShowType,
						   char **varValue)
{
	char		varName[SYSTEM_VAR_NAME_MAX_LEN];

	if (rawVarName == NULL || strlen(rawVarName) >= sizeof(varName))
		return 1;

	if (sessionSystemVars == NULL)
		initSystemVariables();

	strlcpy(varName, rawVarName, sizeof(varName));
	lowerSystemVarName(varName);

	if (isSessionSystemVar)
	{
		SystemVar  *systemVar;
		bool		found;

		systemVar = (SystemVar *) hash_search(sessionSystemVars,
											  varName,
											  HASH_FIND,
											  &found);
		if (found || (systemVar = dynAddSystemVar(varName)) != NULL)
		{
			*varValue = getConfValue(systemVar, selectShowType);
			return 0;
		}
		*varValue = NULL;
		return 1;
	}
	else
	{
		SystemVar	globalCopy;
		bool		found;

		found = lookup_global_system_var(varName, &globalCopy);
		if (!found)
		{
			if (dynAddSystemVar(varName) != NULL)
				found = lookup_global_system_var(varName, &globalCopy);
		}
		if (!found)
		{
			/* Fallback to baseSystemVars if shared memory was not preloaded */
			for (int i = 0; i < baseSystemVarsNum; i++)
			{
				if (strcmp(varName, baseSystemVars[i]->varName) == 0)
				{
					globalCopy = *baseSystemVars[i];
					found = true;
					break;
				}
			}
		}

		if (found)
		{
			if (globalCopy.sessionGlobalType == 0 ||
				globalCopy.sessionGlobalType == 1 ||
				globalCopy.sessionGlobalType == 3)
			{
				*varValue = getConfValue(&globalCopy, selectShowType);
				return 0;
			}
			*varValue = NULL;
			return 2;
		}

		*varValue = NULL;
		return 1;
	}
}

static char *
getConfValue(const SystemVar *systemVar, int selectShowType)
{
	int			varConfValueLen = (int) strlen(systemVar->varConfValue);

	if (selectShowType == 1)
	{
		if (systemVar->selectResultNum == 0)
			return pstrdup(systemVar->varConfValue);

		for (int i = 0; i < systemVar->selectResultNum; i++)
		{
			int			resultItemLen = (int) strlen(systemVar->result[i]);

			if (varConfValueLen == resultItemLen &&
				pg_strncasecmp(systemVar->varConfValue,
							   systemVar->result[i],
							   varConfValueLen) == 0)
				return pstrdup(systemVar->selectResult[i]);
		}
	}
	else
	{
		if (systemVar->showResultNum == 0)
			return pstrdup(systemVar->varConfValue);

		for (int i = 0; i < systemVar->showResultNum; i++)
		{
			int			resultItemLen = (int) strlen(systemVar->result[i]);

			if (varConfValueLen == resultItemLen &&
				pg_strncasecmp(systemVar->varConfValue,
							   systemVar->result[i],
							   varConfValueLen) == 0)
				return pstrdup(systemVar->showResult[i]);
		}
	}

	return pstrdup(systemVar->varConfValue);
}
