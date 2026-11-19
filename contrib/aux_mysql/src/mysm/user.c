/*-------------------------------------------------------------------------
 *
 * user.c
 *	  MySQL CURRENT_USER() and USER() / SESSION_USER() helpers
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include "fmgr.h"
#include "libpq/libpq-be.h"
#include "miscadmin.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(getCurrentUser);
Datum
getCurrentUser(PG_FUNCTION_ARGS)
{
	if (MyProcPort != NULL && MyProcPort->user_name != NULL)
	{
		char	   *userInfo = psprintf("%s@%%", MyProcPort->user_name);
		text	   *res = cstring_to_text(userInfo);

		pfree(userInfo);
		PG_RETURN_TEXT_P(res);
	}

	PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(getSessionUser);
Datum
getSessionUser(PG_FUNCTION_ARGS)
{
	if (MyProcPort != NULL &&
		MyProcPort->user_name != NULL &&
		MyProcPort->remote_host != NULL)
	{
		char	   *userInfo = psprintf("%s@%s",
										MyProcPort->user_name,
										MyProcPort->remote_host);
		text	   *res = cstring_to_text(userInfo);

		pfree(userInfo);
		PG_RETURN_TEXT_P(res);
	}

	PG_RETURN_NULL();
}
