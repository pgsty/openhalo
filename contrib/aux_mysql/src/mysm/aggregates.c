/*-------------------------------------------------------------------------
 *
 * aggregates.c
 *	  MySQL-compatible aggregate transition helpers
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
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

PG_FUNCTION_INFO_V1(textAvgAccum);
Datum
textAvgAccum(PG_FUNCTION_ARGS)
{
	Datum		orgDatum1 = PG_GETARG_DATUM(1);
	Datum		result;

	if (!PG_ARGISNULL(1))
	{
		char	   *num = TextDatumGetCString(orgDatum1);

		fcinfo->args[1].value = DirectFunctionCall3(numeric_in,
													CStringGetDatum(num),
													ObjectIdGetDatum(InvalidOid),
													Int32GetDatum(-1));
		pfree(num);
	}

	result = numeric_avg_accum(fcinfo);
	fcinfo->args[1].value = orgDatum1;

	return result;
}
