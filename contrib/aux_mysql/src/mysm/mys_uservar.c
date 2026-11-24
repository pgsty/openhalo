/*-------------------------------------------------------------------------
 *
 * mys_uservar.c
 *	  MySQL session user-defined variable (@var) storage
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

#include "catalog/pg_type.h"
#include "commands/mysql/mys_uservar.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/varbit.h"

/*
 * MySQL user variable names are case-insensitive and allow up to 64 UTF-8
 * characters (<= 256 bytes plus NUL).
 */
#define MYS_USERVAR_KEYSIZE		257

typedef struct MysqlUserVariableItem
{
	char		varName[MYS_USERVAR_KEYSIZE];
	bytea	   *varValue;
	bool		isDigit;
	bool		isnull;
} MysqlUserVariableItem;

typedef struct PendingUserVar
{
	MemoryContextCallback cleanup;
	char		key[MYS_USERVAR_KEYSIZE];
	bytea	   *value;
	bool		is_digit;
	bool		created;
	MysqlUserVariableItem *entry;
	uint64		generation;
} PendingUserVar;

/*
 * Track bytea* values returned by mysGetUserVarValueInternal() whose stored
 * variable has isDigit = true, using a memory-context reset callback rather
 * than writing out-of-bounds past VARSIZE_ANY().
 */
typedef struct DigitByteaTag
{
	MemoryContextCallback cb;
	const bytea *ptr;
	struct DigitByteaTag *prev;
	struct DigitByteaTag *next;
} DigitByteaTag;

static HTAB *mysql_user_variables = NULL;
static uint64 user_var_generation = 1;
static DigitByteaTag *digit_bytea_head = NULL;

static void
unlink_digit_bytea_tag(void *arg)
{
	DigitByteaTag *tag = (DigitByteaTag *) arg;

	if (tag->prev)
		tag->prev->next = tag->next;
	else
		digit_bytea_head = tag->next;

	if (tag->next)
		tag->next->prev = tag->prev;

	tag->prev = NULL;
	tag->next = NULL;
	tag->ptr = NULL;
}

static void
register_digit_bytea(const bytea *ptr)
{
	DigitByteaTag *tag = (DigitByteaTag *) palloc(sizeof(DigitByteaTag));

	tag->ptr = ptr;
	tag->prev = NULL;
	tag->next = digit_bytea_head;
	if (digit_bytea_head)
		digit_bytea_head->prev = tag;
	digit_bytea_head = tag;

	tag->cb.func = unlink_digit_bytea_tag;
	tag->cb.arg = tag;
	MemoryContextRegisterResetCallback(CurrentMemoryContext, &tag->cb);
}

static bool
is_registered_digit_bytea(const bytea *ptr)
{
	const DigitByteaTag *cur;

	for (cur = digit_bytea_head; cur != NULL; cur = cur->next)
	{
		if (cur->ptr == ptr)
			return true;
	}
	return false;
}

static void
initMysqlUserVariableHashTable(void)
{
	HASHCTL		hash_ctl;

	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = MYS_USERVAR_KEYSIZE;
	hash_ctl.entrysize = sizeof(MysqlUserVariableItem);
	hash_ctl.hcxt = TopMemoryContext;

	mysql_user_variables = hash_create("Mysql User Variables",
									   64,
									   &hash_ctl,
									   HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);
}

static void
normalize_uservar_key(const char *userVarName, char key[MYS_USERVAR_KEYSIZE])
{
	size_t		len;
	size_t		i;

	if (userVarName == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("User variable name cannot be NULL")));

	len = strlen(userVarName);
	if (len >= MYS_USERVAR_KEYSIZE)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("User variable name '%s' is illegal", userVarName)));

	for (i = 0; i < len; i++)
		key[i] = pg_ascii_tolower((unsigned char) userVarName[i]);
	key[len] = '\0';
}

void
clearUserVars(void)
{
	HASH_SEQ_STATUS seq;
	MysqlUserVariableItem *entry;

	if (mysql_user_variables == NULL)
		return;

	hash_seq_init(&seq, mysql_user_variables);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		if (!entry->isnull && entry->varValue != NULL)
		{
			pfree(entry->varValue);
			entry->varValue = NULL;
		}
	}
	user_var_generation++;
	hash_destroy(mysql_user_variables);
	mysql_user_variables = NULL;
}

bool
varValueIsDigit(Datum varValue, Oid varValueType)
{
	char		ncategory;
	bool		nispreferred;

	if (!OidIsValid(varValueType))
		return false;

	if (varValueType == BYTEAOID)
		return is_registered_digit_bytea((const bytea *) DatumGetPointer(varValue));

	if (varValueType == BOOLOID)
		return true;

	/*
	 * BIT / VARBIT and binary-string domains (hexlit, binary, varbinary, blob)
	 * store raw binary bytes rather than ASCII decimal digits.
	 */
	if (varValueType == BITOID || varValueType == VARBITOID ||
		mys_binary_base_type(varValueType) == BYTEAOID)
		return false;

	get_type_category_preferred(varValueType, &ncategory, &nispreferred);
	return (ncategory == TYPCATEGORY_NUMERIC);
}

static MysqlUserVariableItem *
reserve_user_var(const char *key, bool *created)
{
	MysqlUserVariableItem *entry;
	bool		found;

	if (!mysql_user_variables)
		initMysqlUserVariableHashTable();
	entry = hash_search(mysql_user_variables, key, HASH_ENTER, &found);
	if (!found)
	{
		entry->varValue = NULL;
		entry->isnull = true;
		entry->isDigit = false;
	}
	*created = !found;
	return entry;
}

static void
assign_user_var(MysqlUserVariableItem *entry, bytea *value, bool is_digit)
{
	if (entry->varValue)
		pfree(entry->varValue);
	entry->varValue = value;
	entry->isnull = value == NULL;
	entry->isDigit = value != NULL && is_digit;
}

static void
discard_pending_user_var(void *arg)
{
	PendingUserVar *var = arg;

	if (var->value)
		pfree(var->value);
	if (var->created && var->generation == user_var_generation && var->entry->isnull)
		hash_search(mysql_user_variables, var->key, HASH_REMOVE, NULL);
}

void
mys_reserve_user_vars(MysSetBatch *batch)
{
	foreach_ptr(PendingUserVar, var, batch->user_vars)
	{
		if (var->generation != user_var_generation)
		{
			var->entry = reserve_user_var(var->key, &var->created);
			var->generation = user_var_generation;
		}
	}
}

void
mys_publish_user_vars(MysSetBatch *batch)
{
	foreach_ptr(PendingUserVar, var, batch->user_vars)
	{
		assign_user_var(var->entry, var->value, var->is_digit);
		var->value = NULL;
		var->created = false;
	}
}

static void
set_user_var_bytes(MysSetBatch *batch, const char *userVarName, const char *bytes,
				   size_t byteLen, bool isDigit, bool isNull)
{
	char		key[MYS_USERVAR_KEYSIZE];
	PendingUserVar *pending = NULL;
	MysqlUserVariableItem *entry = NULL;
	bytea	   *newVal = NULL;

	normalize_uservar_key(userVarName, key);
	if (batch)
	{
		MemoryContext old = MemoryContextSwitchTo(batch->context);

		pending = palloc0_object(PendingUserVar);
		strlcpy(pending->key, key, sizeof(pending->key));
		pending->is_digit = isDigit;
		pending->cleanup.func = discard_pending_user_var;
		pending->cleanup.arg = pending;
		MemoryContextRegisterResetCallback(batch->context, &pending->cleanup);
		batch->user_vars = lappend(batch->user_vars, pending);
		MemoryContextSwitchTo(old);
	}
	else
	{
		bool		created;

		entry = reserve_user_var(key, &created);
	}

	if (!isNull)
	{
		newVal = (bytea *) MemoryContextAlloc(TopMemoryContext, VARHDRSZ + byteLen);
		SET_VARSIZE(newVal, VARHDRSZ + byteLen);
		if (byteLen > 0)
			memcpy(VARDATA(newVal), bytes, byteLen);
	}
	if (pending)
		pending->value = newVal;
	else
		assign_user_var(entry, newVal, isDigit);
}

void
mysSetUserVarBytes(const char *userVarName, const char *bytes,
				   size_t byteLen, bool isDigit, bool isNull)
{
	set_user_var_bytes(NULL, userVarName, bytes, byteLen, isDigit, isNull);
}

/*
 * Convert a PostgreSQL VarBit (left-aligned MSB-first) into MySQL's
 * right-aligned big-endian byte representation.
 */
static void
varbit_to_mysql_bytes(VarBit *vb, char **out_bytes, size_t *out_len)
{
	int32		bitlen = VARBITLEN(vb);
	size_t		nbytes = (size_t) ((bitlen + 7) / 8);
	uint8	   *inbits = VARBITS(vb);
	uint8	   *buf;
	size_t		pad;
	int32		k;

	if (nbytes == 0)
	{
		*out_bytes = NULL;
		*out_len = 0;
		return;
	}

	buf = (uint8 *) palloc0(nbytes);
	pad = nbytes * BITS_PER_BYTE - (size_t) bitlen;
	for (k = 0; k < bitlen; k++)
	{
		if (inbits[k / BITS_PER_BYTE] & (0x80 >> (k % BITS_PER_BYTE)))
		{
			size_t		pos = pad + (size_t) k;

			buf[pos / BITS_PER_BYTE] |= (uint8) (0x80 >> (pos % BITS_PER_BYTE));
		}
	}

	*out_bytes = (char *) buf;
	*out_len = nbytes;
}

void
mysSetUserVarDatum(MysSetBatch *batch, const char *userVarName, Datum datumValue,
				   Oid varValueType, FmgrInfo *foutcache, bool isNull)
{
	Oid			baseType;
	bool		isDigit;

	if (isNull)
	{
		set_user_var_bytes(batch, userVarName, NULL, 0, false, true);
		return;
	}

	baseType = mys_binary_base_type(varValueType);
	isDigit = varValueIsDigit(datumValue, varValueType);

	if (baseType == BYTEAOID)
	{
		bytea	   *bv = DatumGetByteaPP(datumValue);

		set_user_var_bytes(batch, userVarName, VARDATA_ANY(bv), VARSIZE_ANY_EXHDR(bv),
						   isDigit, false);
	}
	else if (getBaseType(varValueType) == BPCHAROID)
	{
		bytea	   *bv = mys_bpchar_bytes(datumValue);

		set_user_var_bytes(batch, userVarName, VARDATA_ANY(bv), VARSIZE_ANY_EXHDR(bv),
						   false, false);
	}
	else if (baseType == BITOID || baseType == VARBITOID)
	{
		VarBit	   *vb = DatumGetVarBitP(datumValue);
		char	   *bitbytes = NULL;
		size_t		bitlen = 0;

		varbit_to_mysql_bytes(vb, &bitbytes, &bitlen);
		set_user_var_bytes(batch, userVarName, bitbytes, bitlen, false, false);
		if (bitbytes)
			pfree(bitbytes);
	}
	else if (baseType == BOOLOID)
	{
		char		bch = DatumGetBool(datumValue) ? '1' : '0';

		set_user_var_bytes(batch, userVarName, &bch, 1, true, false);
	}
	else
	{
		char	   *str;

		if (foutcache != NULL)
			str = OutputFunctionCall(foutcache, datumValue);
		else
		{
			Oid			typOutput;
			bool		typIsVarlena;

			getTypeOutputInfo(varValueType, &typOutput, &typIsVarlena);
			str = OidOutputFunctionCall(typOutput, datumValue);
		}
		set_user_var_bytes(batch, userVarName, str, strlen(str), isDigit, false);
		pfree(str);
	}
}

void
mysSetUserVarInternal(char *userVarName, char *userVarValue,
					  Oid varValueType, bool isDigit, bool isNull)
{
	Oid			baseVarValueType;

	if (isNull || userVarValue == NULL)
	{
		mysSetUserVarBytes(userVarName, NULL, 0, false, true);
		return;
	}

	baseVarValueType = mys_binary_base_type(varValueType);
	if (baseVarValueType == BYTEAOID)
	{
		Datum		d = DirectFunctionCall1(byteain, CStringGetDatum(userVarValue));
		bytea	   *bv = DatumGetByteaPP(d);

		mysSetUserVarBytes(userVarName, VARDATA_ANY(bv), VARSIZE_ANY_EXHDR(bv),
						   isDigit, false);
	}
	else if (baseVarValueType == BITOID || baseVarValueType == VARBITOID)
	{
		size_t		slen = strlen(userVarValue);
		size_t		nbytes = (slen + 7) / 8;
		uint8	   *buf = nbytes > 0 ? (uint8 *) palloc0(nbytes) : NULL;
		size_t		pad = nbytes * 8 - slen;
		size_t		i;

		for (i = 0; i < slen; i++)
		{
			if (userVarValue[i] == '1')
			{
				size_t		pos = pad + i;

				buf[pos / 8] |= (uint8) (0x80 >> (pos % 8));
			}
			else if (userVarValue[i] != '0')
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
						 errmsg("Invalid input syntax for type bit")));
		}
		mysSetUserVarBytes(userVarName, (const char *) buf, nbytes, false, false);
		if (buf)
			pfree(buf);
	}
	else if (baseVarValueType == BOOLOID)
	{
		char		bch = (userVarValue[0] == '1' ||
						   userVarValue[0] == 't' ||
						   userVarValue[0] == 'T') ? '1' : '0';

		mysSetUserVarBytes(userVarName, &bch, 1, true, false);
	}
	else
	{
		mysSetUserVarBytes(userVarName, userVarValue, strlen(userVarValue),
						   isDigit, false);
	}
}

void
mysSetUserVarForPl(char *userVarName, Datum userVarValue,
				   Oid varValueType, bool isNull)
{
	mysSetUserVarDatum(NULL, userVarName, userVarValue, varValueType, NULL, isNull);
}

bytea *
mysGetUserVarValueInternal(char *userVarName)
{
	char		key[MYS_USERVAR_KEYSIZE];
	MysqlUserVariableItem *entry;
	bool		found;
	size_t		byteLen;
	bytea	   *result;

	if (userVarName == NULL || !mysql_user_variables)
		return NULL;

	normalize_uservar_key(userVarName, key);
	entry = (MysqlUserVariableItem *) hash_search(mysql_user_variables,
												  key,
												  HASH_FIND,
												  &found);
	if (!found || entry->isnull || entry->varValue == NULL)
		return NULL;

	byteLen = VARSIZE_ANY_EXHDR(entry->varValue);
	result = (bytea *) palloc(VARHDRSZ + byteLen);
	SET_VARSIZE(result, VARHDRSZ + byteLen);
	if (byteLen > 0)
		memcpy(VARDATA(result), VARDATA_ANY(entry->varValue), byteLen);

	if (entry->isDigit)
		register_digit_bytea(result);

	return result;
}
