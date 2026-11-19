/*-------------------------------------------------------------------------
 *
 * uuid_short_core.c
 *	  MySQL UUID_SHORT() lock-free shared-memory generator
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mysm_compat.h"

#include <time.h>

#include "adapter/mysql/uuidShort.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/shmem.h"

typedef struct UuidShortSharedState
{
	pg_atomic_uint64 counter;
} UuidShortSharedState;

static UuidShortSharedState *uuid_short_shmem = NULL;
static uint64 local_uuid_short_fallback = 0;

static uint64
compute_initial_uuid_short(uint64 startup_secs)
{
	const uint64 server_id = 1ULL;

	return ((server_id & 0xFFULL) << 56) |
		((startup_secs & 0xFFFFFFFFULL) << 24);
}

Size
UuidShortShmemSize(void)
{
	return MAXALIGN(sizeof(UuidShortSharedState));
}

void
UuidShortShmemInit(void)
{
	bool		found = false;

	uuid_short_shmem = (UuidShortSharedState *)
		ShmemInitStruct("MySQL UUID_SHORT State",
						sizeof(UuidShortSharedState),
						&found);

	if (!found)
	{
		uint64		startup_secs = (PgStartTime > 0) ?
			(uint64) timestamptz_to_time_t(PgStartTime) :
			(uint64) time(NULL);

		pg_atomic_init_u64(&uuid_short_shmem->counter,
						   compute_initial_uuid_short(startup_secs));
	}
}

unsigned long long
getUuidShort(void)
{
	if (uuid_short_shmem != NULL)
		return (unsigned long long) (pg_atomic_fetch_add_u64(&uuid_short_shmem->counter, 1) + 1);

	if (local_uuid_short_fallback == 0)
	{
		uint64		startup_secs = (MyStartTime > 0) ?
			(uint64) MyStartTime : (uint64) time(NULL);

		local_uuid_short_fallback = compute_initial_uuid_short(startup_secs);
	}
	return (unsigned long long) (++local_uuid_short_fallback);
}
