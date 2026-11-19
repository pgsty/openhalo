/*-------------------------------------------------------------------------
 *
 * uuidShort.h
 *	  MySQL UUID_SHORT() shared counter declarations
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 * Portions Copyright (c) 2019-2026, Halo Tech Co.,Ltd.
 * Maintained by Ruohang Feng @ PGSTY (rh@vonng.com)
 *
 *-------------------------------------------------------------------------
 */

#ifndef UUID_SHORT_H
#define UUID_SHORT_H

#include "postgres.h"

extern Size UuidShortShmemSize(void);
extern void UuidShortShmemInit(void);
extern unsigned long long getUuidShort(void);

#endif							/* UUID_SHORT_H */
