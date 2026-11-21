/* Copyright (c) 2026, PGSTY PTE. LTD. */
#ifndef MYS_CURRENT_READ_H
#define MYS_CURRENT_READ_H

#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"

extern void mys_current_read_init(void);
extern void mys_current_read_rewrite_limit(Query *query);
extern void mys_current_read_plan(PlannedStmt *stmt, bool has_row_security);

#endif
