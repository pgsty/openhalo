/*-------------------------------------------------------------------------
 * mys_current_read.c
 *    Current reads for MySQL UPDATE/DELETE by a complete unique key.
 *
 * PostgreSQL READ COMMITTED follows concurrent UPDATE chains, but skips a
 * tuple deleted by another transaction. A DELETE followed by INSERT of the
 * same key creates a new tuple, not an UPDATE chain. A waiting DELETE can
 * therefore report zero rows and a following INSERT fails with 1062.
 *
 * Lock the target before evaluating its quals/projection. If the row was
 * deleted or changed while we waited, repeat only the unique-key lookup with
 * a fresh snapshot. No statement, trigger, or previously completed write is
 * replayed. Real tuple locks also cooperate with native PostgreSQL writers.
 *
 * This deliberately covers single-table point writes on ordinary heap tables
 * with a complete, immediate, non-partial B-tree unique key. It does not add
 * InnoDB gap/range locking. RLS plans retain their original execution order.
 * Transaction-snapshot isolation and native sessions retain PG semantics,
 * including when a cached plan is executed under a different isolation level.
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#include "halo_mysql.h"
#include "mys_current_read.h"

typedef struct MysCurrentRead
{
	CustomScanState css;
	Relation	index;
	IndexScanDesc scan;
	Snapshot	snapshot;
	ScanKey		keys;
	int			nkeys;
	IndexRuntimeKeyInfo *runtime_keys;
	int			nruntime_keys;
	bool		current_read;
	bool		done;
} MysCurrentRead;

static Node *current_read_state(CustomScan *plan);
static void current_read_begin(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *current_read_exec(CustomScanState *node);
static void current_read_end(CustomScanState *node);
static void current_read_rescan(CustomScanState *node);

static const CustomScanMethods scan_methods = {
	.CustomName = "MySQL Current Read",
	.CreateCustomScanState = current_read_state
};

static const CustomExecMethods exec_methods = {
	.CustomName = "MySQL Current Read",
	.BeginCustomScan = current_read_begin,
	.ExecCustomScan = current_read_exec,
	.EndCustomScan = current_read_end,
	.ReScanCustomScan = current_read_rescan
};

void
mys_current_read_init(void)
{
	RegisterCustomScanMethods(&scan_methods);
}

/* Accept only a column = constant/external parameter in the index opfamily. */
static OpExpr *
key_qual(List *quals, Relation index, int key, Index rti)
{
	foreach_ptr(Node, qual, quals)
	{
		OpExpr	   *op;
		Node	   *left;
		Node	   *right;
		Oid			opno;
		Var		   *var;
		OpExpr	   *result;

		if (!IsA(qual, OpExpr) || list_length(((OpExpr *) qual)->args) != 2)
			continue;
		op = (OpExpr *) qual;
		left = linitial(op->args);
		while (IsA(left, RelabelType))
			left = (Node *) ((RelabelType *) left)->arg;
		right = lsecond(op->args);
		opno = op->opno;
		if (!IsA(left, Var))
		{
			left = lsecond(op->args);
			while (IsA(left, RelabelType))
				left = (Node *) ((RelabelType *) left)->arg;
			right = linitial(op->args);
			opno = get_commutator(opno);
		}
		if (!IsA(left, Var) || !OidIsValid(opno) ||
			!(IsA(right, Const) ||
			  (IsA(right, Param) && ((Param *) right)->paramkind == PARAM_EXTERN)))
			continue;
		var = (Var *) left;
		if (var->varno != rti || var->varlevelsup != 0 ||
			var->varattno != index->rd_index->indkey.values[key] ||
			op->inputcollid != index->rd_indcollation[key] ||
			get_op_opfamily_strategy(opno, index->rd_opfamily[key]) != BTEqualStrategyNumber)
			continue;

		result = copyObject(op);
		var = copyObject(var);
		var->varno = INDEX_VAR;
		var->varattno = key + 1;
		result->opno = opno;
		result->opfuncid = get_opcode(opno);
		result->args = list_make2(var, copyObject(right));
		return result;
	}
	return NULL;
}

void
mys_current_read_plan(PlannedStmt *stmt, bool has_row_security)
{
	ModifyTable *modify;
	Scan	   *scan;
	List	   *quals;
	RangeTblEntry *rte;
	Relation	table;
	List	   *indexes;

	if (has_row_security || stmt->hasModifyingCTE ||
		!(stmt->commandType == CMD_UPDATE || stmt->commandType == CMD_DELETE) ||
		!IsA(stmt->planTree, ModifyTable))
		return;
	modify = (ModifyTable *) stmt->planTree;
	if (list_length(modify->resultRelations) != 1 ||
		!(IsA(outerPlan(modify), IndexScan) || IsA(outerPlan(modify), SeqScan) ||
		  IsA(outerPlan(modify), BitmapHeapScan)))
		return;
	scan = (Scan *) outerPlan(modify);
	if (scan->scanrelid != linitial_int(modify->resultRelations))
		return;
	quals = list_copy(scan->plan.qual);
	if (IsA(scan, IndexScan))
		quals = list_concat(quals, list_copy(((IndexScan *) scan)->indexqualorig));
	else if (IsA(scan, BitmapHeapScan))
		quals = list_concat(quals, list_copy(((BitmapHeapScan *) scan)->bitmapqualorig));
	rte = list_nth_node(RangeTblEntry, stmt->rtable, scan->scanrelid - 1);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
		return;
	table = table_open(rte->relid, NoLock);
	if (table->rd_rel->relam != HEAP_TABLE_AM_OID)
	{
		table_close(table, NoLock);
		return;
	}
	indexes = RelationGetIndexList(table);
	foreach_oid(indexid, indexes)
	{
		Relation	index = index_open(indexid, AccessShareLock);
		List	   *keys = NIL;
		int			nkeys = index->rd_index->indnkeyatts;

		if (index->rd_rel->relam == BTREE_AM_OID &&
			index->rd_index->indisunique && index->rd_index->indimmediate &&
			index->rd_index->indisvalid && !index->rd_index->indcheckxmin &&
			RelationGetIndexPredicate(index) == NIL &&
			RelationGetIndexExpressions(index) == NIL)
		{
			for (int i = 0; i < nkeys; i++)
			{
				OpExpr *qual = key_qual(quals, index, i, scan->scanrelid);

				if (qual == NULL)
					break;
				keys = lappend(keys, qual);
			}
		}
		if (list_length(keys) == nkeys && nkeys > 0)
		{
			CustomScan *current = makeNode(CustomScan);

			current->scan = *scan;
			NodeSetTag(current, T_CustomScan);
			current->scan.plan.qual = quals;
			/* The unique-key lookup replaces the bitmap producer as well. */
			outerPlan(current) = NULL;
			innerPlan(current) = NULL;
			current->custom_private = list_make1(makeInteger(indexid));
			current->custom_exprs = keys;
			current->custom_relids = bms_make_singleton(scan->scanrelid);
			current->methods = &scan_methods;
			outerPlan(modify) = (Plan *) current;
			/* Keep the chosen index locked, also when replacing a SeqScan. */
			index_close(index, NoLock);
			break;
		}
		index_close(index, AccessShareLock);
	}
	list_free(indexes);
	table_close(table, NoLock);
}

static Node *
current_read_state(CustomScan *plan)
{
	MysCurrentRead *node = palloc0(sizeof(MysCurrentRead));

	NodeSetTag(node, T_CustomScanState);
	node->css.methods = &exec_methods;
	node->css.slotOps = &TTSOpsBufferHeapTuple;
	return (Node *) node;
}

static void
current_read_begin(CustomScanState *css, EState *estate, int eflags)
{
	MysCurrentRead *node = (MysCurrentRead *) css;
	CustomScan *plan = (CustomScan *) css->ss.ps.plan;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
	node->index = index_open(intVal(linitial(plan->custom_private)),
							 exec_rt_fetch(plan->scan.scanrelid, estate)->rellockmode);
	node->current_read = mys_is_mysql_session() && !IsolationUsesXactSnapshot();
	ExecIndexBuildScanKeys(&css->ss.ps, node->index, plan->custom_exprs, false,
						   &node->keys, &node->nkeys,
						   &node->runtime_keys, &node->nruntime_keys, NULL, NULL);
}

static void
close_scan(MysCurrentRead *node)
{
	if (node->scan)
		index_endscan(node->scan);
	node->scan = NULL;
	if (node->snapshot)
		UnregisterSnapshot(node->snapshot);
	node->snapshot = NULL;
}

static TupleTableSlot *
current_read_next(ScanState *ss)
{
	MysCurrentRead *node = (MysCurrentRead *) ss;
	EState	   *estate = ss->ps.state;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	while (!node->done)
	{
		TM_FailureData tmfd;
		TM_Result	result;
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();
		if (node->scan == NULL)
		{
			ExecIndexEvalRuntimeKeys(ss->ps.ps_ExprContext,
									node->runtime_keys, node->nruntime_keys);
			node->snapshot = RegisterSnapshot(node->current_read ?
											  GetLatestSnapshot() : estate->es_snapshot);
			/* Refresh other transactions, not this command's own visibility. */
			if (node->current_read)
				node->snapshot->curcid = estate->es_snapshot->curcid;
			node->scan = index_beginscan(ss->ss_currentRelation, node->index,
									   node->snapshot, NULL, node->nkeys, 0, SO_NONE);
			index_rescan(node->scan, node->keys, node->nkeys, NULL, 0);
		}
		if (!index_getnext_slot(node->scan, ForwardScanDirection, slot))
			break;
		if (!node->current_read)
		{
			node->done = true;
			return slot;
		}
		tid = slot->tts_tid;
		result = table_tuple_lock(ss->ss_currentRelation, &tid, node->snapshot,
								  slot, estate->es_output_cid,
								  LockTupleExclusive, LockWaitBlock,
								  TUPLE_LOCK_FLAG_FIND_LAST_VERSION, &tmfd);
		if (result == TM_Ok && !tmfd.traversed)
		{
			node->done = true;
			return slot;
		}
		if (result == TM_SelfModified)
			break;
		if (result != TM_Ok && result != TM_Updated && result != TM_Deleted)
			elog(ERROR, "unexpected current-read tuple lock status: %u", result);
		/* Also recheck the key after UPDATE changed it during the wait. */
		ExecClearTuple(slot);
		close_scan(node);
	}
	node->done = true;
	return ExecClearTuple(slot);
}

static bool
current_read_recheck(ScanState *ss, TupleTableSlot *slot)
{
	/* The complete key and all remaining quals are in the scan's qual list. */
	return true;
}

static TupleTableSlot *
current_read_exec(CustomScanState *node)
{
	return ExecScan(&node->ss, current_read_next, current_read_recheck);
}

static void
current_read_end(CustomScanState *css)
{
	MysCurrentRead *node = (MysCurrentRead *) css;

	close_scan(node);
	if (node->index)
		index_close(node->index, NoLock);
	ExecClearTuple(css->ss.ss_ScanTupleSlot);
}

static void
current_read_rescan(CustomScanState *css)
{
	MysCurrentRead *node = (MysCurrentRead *) css;

	close_scan(node);
	node->done = false;
	ExecScanReScan(&css->ss);
}
