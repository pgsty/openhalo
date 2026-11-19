/*-------------------------------------------------------------------------
 *
 * uuid_short.c
 *	  MySQL UUID_SHORT() SQL entry point
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "adapter/mysql/uuidShort.h"
#include "fmgr.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(uuidShort);
Datum
uuidShort(PG_FUNCTION_ARGS)
{
	char		ret[32];
	unsigned long long val = getUuidShort();

	snprintf(ret, sizeof(ret), "%llu", val);
	PG_RETURN_TEXT_P(cstring_to_text(ret));
}
