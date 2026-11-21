/*-------------------------------------------------------------------------
 * mys_current_read.c
 *    Current reads for MySQL UPDATE/DELETE by a complete unique key.
 *
 * PostgreSQL READ COMMITTED follows concurrent UPDATE chains, but skips a
 * tuple deleted by another transaction. A DELETE followed by INSERT of the
 * same key creates a new tuple, not an UPDATE chain. A waiting DELETE can
 * therefore report zero rows and a following INSERT fails with 1062.
 *
 * Uncontended target tuples take a zero-prelock fast path using es_snapshot
 * and let ExecModifyTable modify the row directly (emitting no extra
 * XLOG_HEAP_LOCK record and acquiring no tuple lock when residual quals fail).
 * If the target tuple is contended—or races with a concurrent deletion/update
 * before ExecModifyTable locks it—we repeat only the unique-key lookup with a
 * fresh snapshot (preserving command curcid) and lock with ExecUpdateLockMode
 * and flags = 0 (avoiding cross-row locks when a concurrent UPDATE moved the
 * key).
 *
 * This deliberately covers single-table point writes on ordinary heap tables
 * and partitioned heap tables with a complete, immediate, non-partial B-tree
 * unique key. It does not add InnoDB gap/range locking. RLS plans retain
 * their original execution order. Transaction-snapshot isolation and native
 * sessions retain PG semantics, including when a cached plan is executed
 * under a different isolation level.
 *
 * Copyright (c) 2026, PGSTY PTE. LTD.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "commands/trigger.h"
#include "executor/executor.h"
#include "executor/nodeIndexscan.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteManip.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

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
	ExprContext *runtime_context;
	LockTupleMode lockmode;
	uint64		prev_processed;
	ItemPointerData deferred_tid;
	bool		current_read;
	bool		can_defer_lock;
	bool		deferred_check;
	bool		use_latest_snapshot;
	bool		keys_evaluated;
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

static Node *
strip_relabel(Node *node)
{
	while (node && IsA(node, RelabelType))
		node = (Node *) ((RelabelType *) node)->arg;
	return node;
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
		Node	   *right_bare;
		Oid			opno;
		Var		   *var;
		OpExpr	   *result;

		if (!IsA(qual, OpExpr) || list_length(((OpExpr *) qual)->args) != 2)
			continue;
		op = (OpExpr *) qual;
		left = strip_relabel(linitial(op->args));
		right = lsecond(op->args);
		opno = op->opno;
		if (!IsA(left, Var))
		{
			left = strip_relabel(lsecond(op->args));
			right = linitial(op->args);
			opno = get_commutator(opno);
		}
		right_bare = strip_relabel(right);
		if (!IsA(left, Var) || !OidIsValid(opno) ||
			!(IsA(right_bare, Const) ||
			  (IsA(right_bare, Param) &&
			   ((Param *) right_bare)->paramkind == PARAM_EXTERN)))
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

static bool
is_valid_unique_btree_index(Relation index, char expected_relkind)
{
	return index->rd_rel->relkind == expected_relkind &&
		index->rd_rel->relam == BTREE_AM_OID &&
		index->rd_index->indisunique && index->rd_index->indimmediate &&
		index->rd_index->indisvalid && !index->rd_index->indcheckxmin &&
		index->rd_index->indnkeyatts > 0 &&
		RelationGetIndexPredicate(index) == NIL &&
		RelationGetIndexExpressions(index) == NIL;
}

static List *
match_all_index_key_quals(Relation index, Index rti, List *quals)
{
	List	   *keys = NIL;
	int			nkeys = index->rd_index->indnkeyatts;

	for (int i = 0; i < nkeys; i++)
	{
		OpExpr	   *qual = key_qual(quals, index, i, rti);

		if (qual == NULL)
		{
			list_free_deep(keys);
			return NIL;
		}
		keys = lappend(keys, qual);
	}
	return keys;
}

/*
 * Find a valid, immediate, ordinary B-tree unique index on relid whose key
 * columns all have equality quals in quals.  If keep_index_lock is true, the
 * chosen index's AccessShareLock is retained on success for plan invalidation.
 */
static bool
find_unique_key_match(Oid relid, Index rti, List *quals, bool keep_index_lock,
					  Oid *chosen_indexid, List **chosen_keys)
{
	Relation	table;
	List	   *indexes;
	bool		found = false;

	table = table_open(relid, AccessShareLock);
	if (table->rd_rel->relkind != RELKIND_RELATION ||
		table->rd_rel->relam != HEAP_TABLE_AM_OID ||
		table->rd_rel->relrowsecurity)
	{
		table_close(table, AccessShareLock);
		return false;
	}
	indexes = RelationGetIndexList(table);
	foreach_oid(indexid, indexes)
	{
		Relation	index = index_open(indexid, AccessShareLock);
		List	   *keys = NIL;

		if (is_valid_unique_btree_index(index, RELKIND_INDEX))
			keys = match_all_index_key_quals(index, rti, quals);
		if (keys != NIL)
		{
			if (chosen_indexid)
				*chosen_indexid = indexid;
			if (chosen_keys)
				*chosen_keys = keys;
			else
				list_free_deep(keys);
			index_close(index, keep_index_lock ? NoLock : AccessShareLock);
			found = true;
			break;
		}
		index_close(index, AccessShareLock);
	}
	list_free(indexes);
	table_close(table, AccessShareLock);
	return found;
}

/*
 * Verify every member of a partitioned hierarchy is either a partitioned
 * table or an attached ordinary heap partition without row-level security.
 */
static bool
partition_hierarchy_all_safe_heap(Oid root_relid)
{
	List	   *inhoids = find_all_inheritors(root_relid, AccessShareLock, NULL);
	bool		ok = true;

	foreach_oid(oid, inhoids)
	{
		Relation	rel = table_open(oid, NoLock);
		char		kind = rel->rd_rel->relkind;

		if (rel->rd_rel->relrowsecurity ||
			!(kind == RELKIND_PARTITIONED_TABLE ||
			  (kind == RELKIND_RELATION &&
			   (oid == root_relid || rel->rd_rel->relispartition) &&
			   rel->rd_rel->relam == HEAP_TABLE_AM_OID)))
			ok = false;
		table_close(rel, NoLock);
		if (!ok)
			break;
	}
	list_free(inhoids);
	return ok;
}

/*
 * Prove global uniqueness on a partitioned table from root-level quals: all
 * partitions must be safe heap partitions and quals must match every key
 * column of a valid, immediate RELKIND_PARTITIONED_INDEX B-tree unique index.
 */
static bool
find_root_partitioned_unique_key_match(Oid root_relid, Index rti, List *quals)
{
	Relation	table;
	List	   *indexes;
	bool		found = false;

	if (!partition_hierarchy_all_safe_heap(root_relid))
		return false;

	table = table_open(root_relid, AccessShareLock);
	if (table->rd_rel->relkind != RELKIND_PARTITIONED_TABLE ||
		table->rd_rel->relrowsecurity)
	{
		table_close(table, AccessShareLock);
		return false;
	}
	indexes = RelationGetIndexList(table);
	foreach_oid(indexid, indexes)
	{
		Relation	index = index_open(indexid, AccessShareLock);
		List	   *keys = NIL;

		if (is_valid_unique_btree_index(index, RELKIND_PARTITIONED_INDEX))
			keys = match_all_index_key_quals(index, rti, quals);
		if (keys != NIL)
		{
			list_free_deep(keys);
			index_close(index, AccessShareLock);
			found = true;
			break;
		}
		index_close(index, AccessShareLock);
	}
	list_free(indexes);
	table_close(table, AccessShareLock);
	return found;
}

typedef struct RootUniqueIndexInfo
{
	Oid			indexid;
	int			nkeys;
} RootUniqueIndexInfo;

static List *
collect_valid_root_unique_indexes(Oid root_relid)
{
	Relation	table;
	List	   *indexes;
	List	   *result = NIL;

	table = table_open(root_relid, AccessShareLock);
	if (table->rd_rel->relkind != RELKIND_PARTITIONED_TABLE ||
		table->rd_rel->relrowsecurity)
	{
		table_close(table, AccessShareLock);
		return NIL;
	}
	indexes = RelationGetIndexList(table);
	foreach_oid(indexid, indexes)
	{
		Relation	index = index_open(indexid, AccessShareLock);

		if (is_valid_unique_btree_index(index, RELKIND_PARTITIONED_INDEX))
		{
			RootUniqueIndexInfo *info = palloc(sizeof(RootUniqueIndexInfo));

			info->indexid = indexid;
			info->nkeys = index->rd_index->indnkeyatts;
			result = lappend(result, info);
		}
		index_close(index, AccessShareLock);
	}
	list_free(indexes);
	table_close(table, AccessShareLock);
	return result;
}

/*
 * Match a leaf partition's translated Scan quals against a valid attached
 * partition unique index that descends from required_root_indexid.
 */
static bool
find_leaf_partition_unique_key_match(Oid leaf_relid, Index rti, List *quals,
									 Oid required_root_indexid,
									 int required_nkeys,
									 Oid *chosen_leaf_indexid,
									 List **chosen_keys)
{
	Relation	table;
	List	   *indexes;
	bool		found = false;

	table = table_open(leaf_relid, AccessShareLock);
	if (table->rd_rel->relkind != RELKIND_RELATION ||
		!table->rd_rel->relispartition ||
		table->rd_rel->relam != HEAP_TABLE_AM_OID ||
		table->rd_rel->relrowsecurity)
	{
		table_close(table, AccessShareLock);
		return false;
	}
	indexes = RelationGetIndexList(table);
	foreach_oid(indexid, indexes)
	{
		Relation	index = index_open(indexid, AccessShareLock);
		List	   *keys = NIL;

		if (index->rd_rel->relispartition &&
			is_valid_unique_btree_index(index, RELKIND_INDEX) &&
			index->rd_index->indnkeyatts == required_nkeys)
		{
			List	   *ancestors = get_partition_ancestors(indexid);

			if (list_member_oid(ancestors, required_root_indexid))
				keys = match_all_index_key_quals(index, rti, quals);
			list_free(ancestors);
		}
		if (keys != NIL)
		{
			*chosen_leaf_indexid = indexid;
			*chosen_keys = keys;
			index_close(index, AccessShareLock);
			found = true;
			break;
		}
		index_close(index, AccessShareLock);
	}
	list_free(indexes);
	table_close(table, AccessShareLock);
	return found;
}

static bool
is_sysvar_sublink_eq(Node *expr, Index varno, AttrNumber attno,
					 Oid expected_opfuncid, int paramid)
{
	OpExpr	   *op;
	Node	   *l;
	Node	   *r;
	Var		   *v;
	Param	   *p;

	if (!expr || !IsA(expr, OpExpr))
		return false;
	op = (OpExpr *) expr;
	if (get_opcode(op->opno) != expected_opfuncid || list_length(op->args) != 2)
		return false;
	l = linitial(op->args);
	r = lsecond(op->args);
	if (!IsA(l, Var) || !IsA(r, Param))
		return false;
	v = (Var *) l;
	p = (Param *) r;
	return v->varno == varno && v->varattno == attno && v->varlevelsup == 0 &&
		p->paramkind == PARAM_SUBLINK && p->paramid == paramid;
}

static bool
is_target_sysvar(TargetEntry *tle, Index varno, AttrNumber attno, AttrNumber resno)
{
	Var		   *v;

	if (!tle || tle->resjunk || tle->resno != resno ||
		!tle->expr || !IsA(tle->expr, Var))
		return false;
	v = (Var *) tle->expr;
	return v->varno == varno && v->varattno == attno && v->varlevelsup == 0;
}

typedef struct RelVarAttnosCtx
{
	Index		varno;
	int			sublevels_up;
	Bitmapset  *varattnos;
} RelVarAttnosCtx;

static bool
collect_rel_varattnos_walker(Node *node, RelVarAttnosCtx *ctx)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var		   *var = (Var *) node;

		if (var->varno == ctx->varno && var->varlevelsup == ctx->sublevels_up)
			ctx->varattnos = bms_add_member(ctx->varattnos,
											var->varattno - FirstLowInvalidHeapAttributeNumber);
		return false;
	}
	if (IsA(node, Query))
	{
		bool		result;

		ctx->sublevels_up++;
		result = query_tree_walker((Query *) node,
								   collect_rel_varattnos_walker,
								   ctx, 0);
		ctx->sublevels_up--;
		return result;
	}
	return expression_tree_walker(node, collect_rel_varattnos_walker, ctx);
}

/*
 * Unwrap mys_gram.y's makeLimitedModifyQual() wrapper:
 *   WHERE (tableoid, ctid) = ANY (SELECT tableoid, ctid FROM rel WHERE <qual> LIMIT N)
 * when rel is a single non-inherited heap table, there is no ORDER BY or OFFSET,
 * N >= 1 is a positive integer constant, and <qual> proves equality on a
 * complete unique key (so at most one row can ever match).
 */
void
mys_current_read_rewrite_limit(Query *query)
{
	RangeTblEntry *rte;
	RangeTblEntry *subrte;
	SubLink    *sl;
	BoolExpr   *test;
	Query	   *sub;
	Node	   *folded_limit;
	List	   *sub_quals;
	Node	   *lifted;
	RTEPermissionInfo *outer_perm;
	RTEPermissionInfo *sub_perm;

	if (!query ||
		!(query->commandType == CMD_UPDATE || query->commandType == CMD_DELETE) ||
		query->hasRowSecurity || query->hasModifyingCTE ||
		query->cteList != NIL || query->withCheckOptions != NIL ||
		query->jointree == NULL ||
		list_length(query->rtable) != 1 ||
		list_length(query->jointree->fromlist) != 1 ||
		!IsA(linitial(query->jointree->fromlist), RangeTblRef) ||
		((RangeTblRef *) linitial(query->jointree->fromlist))->rtindex != query->resultRelation)
		return;

	rte = rt_fetch(query->resultRelation, query->rtable);
	if (rte->rtekind != RTE_RELATION ||
		rte->tablesample != NULL || rte->securityQuals != NIL)
		return;
	if (rte->relkind == RELKIND_RELATION)
	{
		if (has_subclass(rte->relid))
			return;
	}
	else if (rte->relkind != RELKIND_PARTITIONED_TABLE)
		return;

	if (query->jointree->quals == NULL || !IsA(query->jointree->quals, SubLink))
		return;
	sl = (SubLink *) query->jointree->quals;
	if (sl->subLinkType != ANY_SUBLINK ||
		list_length(sl->operName) != 1 ||
		strcmp(strVal(linitial(sl->operName)), "=") != 0 ||
		sl->testexpr == NULL || !IsA(sl->testexpr, BoolExpr) ||
		sl->subselect == NULL || !IsA(sl->subselect, Query))
		return;

	test = (BoolExpr *) sl->testexpr;
	if (test->boolop != AND_EXPR || list_length(test->args) != 2 ||
		!is_sysvar_sublink_eq(linitial(test->args), query->resultRelation,
							  TableOidAttributeNumber, F_OIDEQ, 1) ||
		!is_sysvar_sublink_eq(lsecond(test->args), query->resultRelation,
							  SelfItemPointerAttributeNumber, F_TIDEQ, 2))
		return;

	sub = (Query *) sl->subselect;
	if (sub->commandType != CMD_SELECT ||
		sub->hasAggs || sub->hasWindowFuncs || sub->hasTargetSRFs ||
		sub->hasModifyingCTE || sub->hasDistinctOn || sub->hasForUpdate ||
		sub->hasRowSecurity || sub->cteList != NIL ||
		sub->setOperations != NULL || sub->groupClause != NIL ||
		sub->groupingSets != NIL || sub->havingQual != NULL ||
		sub->distinctClause != NIL || sub->windowClause != NIL ||
		sub->sortClause != NIL || sub->rowMarks != NIL ||
		sub->limitOffset != NULL || sub->limitOption != LIMIT_OPTION_COUNT ||
		sub->limitCount == NULL ||
		list_length(sub->rtable) != 1 ||
		sub->jointree == NULL ||
		list_length(sub->jointree->fromlist) != 1 ||
		!IsA(linitial(sub->jointree->fromlist), RangeTblRef) ||
		((RangeTblRef *) linitial(sub->jointree->fromlist))->rtindex != 1 ||
		list_length(sub->targetList) != 2 ||
		!is_target_sysvar(linitial_node(TargetEntry, sub->targetList),
						  1, TableOidAttributeNumber, 1) ||
		!is_target_sysvar(lsecond_node(TargetEntry, sub->targetList),
						  1, SelfItemPointerAttributeNumber, 2))
		return;

	subrte = rt_fetch(1, sub->rtable);
	if (subrte->rtekind != RTE_RELATION || subrte->relid != rte->relid ||
		subrte->tablesample != NULL || subrte->securityQuals != NIL ||
		sub->jointree->quals == NULL ||
		contain_vars_of_level(sub->jointree->quals, 1) ||
		checkExprHasSubLink(sub->jointree->quals))
		return;

	folded_limit = eval_const_expressions(NULL, sub->limitCount);
	if (!IsA(folded_limit, Const) ||
		((Const *) folded_limit)->constisnull ||
		((Const *) folded_limit)->consttype != INT8OID ||
		DatumGetInt64(((Const *) folded_limit)->constvalue) < 1)
		return;

	/*
	 * Note: make_ands_implicit() returns ((BoolExpr *) qual)->args directly
	 * without copying when the top-level qual is an AND_EXPR, so never pass
	 * the returned List to list_free() on the live sub->jointree->quals tree.
	 */
	sub_quals = make_ands_implicit((Expr *) copyObject(sub->jointree->quals));
	if (rte->relkind == RELKIND_PARTITIONED_TABLE ?
		!find_root_partitioned_unique_key_match(rte->relid, 1, sub_quals) :
		!find_unique_key_match(rte->relid, 1, sub_quals, false, NULL, NULL))
		return;

	lifted = copyObject(sub->jointree->quals);
	if (query->resultRelation != 1)
		ChangeVarNodes(lifted, 1, query->resultRelation, 0);

	/*
	 * Preserve SELECT column permissions recorded on the inner RTE, while
	 * clearing synthetic tableoid/ctid bits added by makeLimitedModifyQual
	 * unless the user query itself referenced them (including via correlated
	 * SubLinks inside targetList or returningList).
	 */
	if (subrte->perminfoindex > 0 && rte->perminfoindex > 0)
	{
		RelVarAttnosCtx vctx = {
			.varno = query->resultRelation,
			.sublevels_up = 0,
			.varattnos = NULL
		};
		int			toid_bit = TableOidAttributeNumber - FirstLowInvalidHeapAttributeNumber;
		int			ctid_bit = SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber;

		collect_rel_varattnos_walker(lifted, &vctx);
		collect_rel_varattnos_walker((Node *) query->targetList, &vctx);
		collect_rel_varattnos_walker((Node *) query->returningList, &vctx);

		outer_perm = getRTEPermissionInfo(query->rteperminfos, rte);
		sub_perm = getRTEPermissionInfo(sub->rteperminfos, subrte);
		outer_perm->requiredPerms |= sub_perm->requiredPerms;
		outer_perm->selectedCols = bms_add_members(outer_perm->selectedCols,
												   sub_perm->selectedCols);
		if (!bms_is_member(toid_bit, vctx.varattnos) && !bms_is_member(0, vctx.varattnos))
			outer_perm->selectedCols = bms_del_member(outer_perm->selectedCols, toid_bit);
		if (!bms_is_member(ctid_bit, vctx.varattnos) && !bms_is_member(0, vctx.varattnos))
			outer_perm->selectedCols = bms_del_member(outer_perm->selectedCols, ctid_bit);
		bms_free(vctx.varattnos);
	}

	query->jointree->quals = lifted;
	query->hasSubLinks = checkExprHasSubLink((Node *) query);
}

static bool
is_supported_leaf_scan(Plan *plan)
{
	return plan != NULL && !plan->parallel_aware &&
		(IsA(plan, IndexScan) || IsA(plan, SeqScan) || IsA(plan, BitmapHeapScan));
}

static List *
extract_leaf_scan_quals(Scan *scan)
{
	List	   *quals = list_copy(scan->plan.qual);

	if (IsA(scan, IndexScan))
		quals = list_concat(quals, list_copy(((IndexScan *) scan)->indexqualorig));
	else if (IsA(scan, BitmapHeapScan))
		quals = list_concat(quals, list_copy(((BitmapHeapScan *) scan)->bitmapqualorig));
	return quals;
}

static CustomScan *
make_current_read_custom_scan(Scan *scan, List *quals, Oid indexid,
							  List *keys, bool allow_defer)
{
	CustomScan *current = makeNode(CustomScan);

	current->scan = *scan;
	NodeSetTag(current, T_CustomScan);
	current->scan.plan.qual = quals;
	/* The unique-key lookup replaces the bitmap producer as well. */
	outerPlan(current) = NULL;
	innerPlan(current) = NULL;
	current->custom_private = list_make2(makeInteger((int) indexid),
										 makeBoolean(allow_defer));
	current->custom_exprs = keys;
	current->custom_relids = bms_make_singleton(scan->scanrelid);
	current->methods = &scan_methods;
	return current;
}

static void
plan_partitioned_current_read(PlannedStmt *stmt, ModifyTable *modify)
{
	RangeTblEntry *root_rte;
	List	   *root_uqs;
	Plan	   *subplan = outerPlan(modify);

	root_rte = list_nth_node(RangeTblEntry, stmt->rtable, modify->rootRelation - 1);
	if (root_rte->rtekind != RTE_RELATION ||
		root_rte->relkind != RELKIND_PARTITIONED_TABLE ||
		!partition_hierarchy_all_safe_heap(root_rte->relid))
		return;

	root_uqs = collect_valid_root_unique_indexes(root_rte->relid);
	if (root_uqs == NIL)
		return;

	/*
	 * Partitioned writes always pre-lock in MysCurrentRead (allow_defer = false)
	 * so root statement triggers, row-movement DELETE/INSERT triggers, and
	 * concurrent cross-partition updates never enter deferred retry or EPQ races.
	 */
	if (list_length(modify->resultRelations) == 1 && is_supported_leaf_scan(subplan))
	{
		Scan	   *scan = (Scan *) subplan;
		RangeTblEntry *rte;

		if (scan->scanrelid != linitial_int(modify->resultRelations))
			return;
		rte = list_nth_node(RangeTblEntry, stmt->rtable, scan->scanrelid - 1);
		if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
			return;

		foreach_ptr(RootUniqueIndexInfo, ruq, root_uqs)
		{
			List	   *quals = extract_leaf_scan_quals(scan);
			Oid			leaf_indexid = InvalidOid;
			List	   *keys = NIL;

			if (find_leaf_partition_unique_key_match(rte->relid, scan->scanrelid,
													 quals, ruq->indexid,
													 ruq->nkeys,
													 &leaf_indexid, &keys))
			{
				LockRelationOid(ruq->indexid, AccessShareLock);
				LockRelationOid(leaf_indexid, AccessShareLock);
				stmt->relationOids = list_append_unique_oid(stmt->relationOids,
															ruq->indexid);
				stmt->relationOids = list_append_unique_oid(stmt->relationOids,
															leaf_indexid);
				outerPlan(modify) = (Plan *)
					make_current_read_custom_scan(scan, quals, leaf_indexid,
												  keys, false);
				return;
			}
			list_free(quals);
		}
		return;
	}

	if (IsA(subplan, Append))
	{
		Append	   *aplan = (Append *) subplan;
		int			nleaves = list_length(aplan->appendplans);
		Bitmapset  *seen_rtis = NULL;

		if (nleaves <= 0 ||
			nleaves != list_length(modify->resultRelations) ||
			aplan->plan.parallel_aware ||
			aplan->nasyncplans != 0 ||
			aplan->first_partial_plan != nleaves)
			return;

		foreach_ptr(Plan, child, aplan->appendplans)
		{
			Scan	   *scan;
			RangeTblEntry *rte;

			if (!is_supported_leaf_scan(child))
				return;
			scan = (Scan *) child;
			if (!list_member_int(modify->resultRelations, scan->scanrelid) ||
				bms_is_member(scan->scanrelid, seen_rtis))
				return;
			seen_rtis = bms_add_member(seen_rtis, scan->scanrelid);
			rte = list_nth_node(RangeTblEntry, stmt->rtable, scan->scanrelid - 1);
			if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION)
				return;
		}

		foreach_ptr(RootUniqueIndexInfo, ruq, root_uqs)
		{
			CustomScan **replacements = palloc0(sizeof(CustomScan *) * nleaves);
			Oid		   *leaf_indexids = palloc0(sizeof(Oid) * nleaves);
			bool		all_matched = true;
			int			idx = 0;

			foreach_ptr(Plan, child, aplan->appendplans)
			{
				Scan	   *scan = (Scan *) child;
				RangeTblEntry *rte = list_nth_node(RangeTblEntry, stmt->rtable,
												   scan->scanrelid - 1);
				List	   *quals = extract_leaf_scan_quals(scan);
				Oid			leaf_indexid = InvalidOid;
				List	   *keys = NIL;

				if (!find_leaf_partition_unique_key_match(rte->relid,
														  scan->scanrelid,
														  quals, ruq->indexid,
														  ruq->nkeys,
														  &leaf_indexid, &keys))
				{
					list_free(quals);
					all_matched = false;
					break;
				}
				leaf_indexids[idx] = leaf_indexid;
				replacements[idx] = make_current_read_custom_scan(scan, quals,
																  leaf_indexid,
																  keys, false);
				idx++;
			}

			if (all_matched)
			{
				ListCell   *lc;

				LockRelationOid(ruq->indexid, AccessShareLock);
				stmt->relationOids = list_append_unique_oid(stmt->relationOids,
															ruq->indexid);
				idx = 0;
				foreach(lc, aplan->appendplans)
				{
					LockRelationOid(leaf_indexids[idx], AccessShareLock);
					stmt->relationOids = list_append_unique_oid(stmt->relationOids,
																leaf_indexids[idx]);
					lfirst(lc) = replacements[idx];
					idx++;
				}
				return;
			}
		}
	}
}

void
mys_current_read_plan(PlannedStmt *stmt, bool has_row_security)
{
	ModifyTable *modify;
	Scan	   *scan;
	List	   *quals;
	RangeTblEntry *rte;
	Oid			indexid = InvalidOid;
	List	   *keys = NIL;

	if (has_row_security || stmt->hasModifyingCTE ||
		!(stmt->commandType == CMD_UPDATE || stmt->commandType == CMD_DELETE) ||
		!IsA(stmt->planTree, ModifyTable))
		return;
	foreach_node(RangeTblEntry, r, stmt->rtable)
	{
		if (r->securityQuals != NIL)
			return;
	}
	modify = (ModifyTable *) stmt->planTree;
	if (modify->rootRelation > 0)
	{
		plan_partitioned_current_read(stmt, modify);
		return;
	}
	if (list_length(modify->resultRelations) != 1 ||
		!is_supported_leaf_scan(outerPlan(modify)))
		return;
	scan = (Scan *) outerPlan(modify);
	if (scan->scanrelid != linitial_int(modify->resultRelations))
		return;
	rte = list_nth_node(RangeTblEntry, stmt->rtable, scan->scanrelid - 1);
	if (rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_RELATION ||
		has_subclass(rte->relid))
		return;
	quals = extract_leaf_scan_quals(scan);

	if (find_unique_key_match(rte->relid, scan->scanrelid, quals, true,
							  &indexid, &keys))
	{
		bool		allow_defer = !modify->onConflictIgnore && modify->canSetTag &&
			!contain_volatile_functions((Node *) scan->plan.targetlist) &&
			!contain_volatile_functions((Node *) quals);

		stmt->relationOids = list_append_unique_oid(stmt->relationOids, indexid);
		outerPlan(modify) = (Plan *)
			make_current_read_custom_scan(scan, quals, indexid, keys, allow_defer);
	}
}

static Oid
lookup_mysql_trigger_fn(const char *proname)
{
	Oid			nspid = get_namespace_oid("mysql", true);
	oidvector  *noargs;

	if (!OidIsValid(nspid))
		return InvalidOid;
	noargs = buildoidvector(NULL, 0);
	return GetSysCacheOid3(PROCNAMEARGSNSP, Anum_pg_proc_oid,
						   CStringGetDatum(proname),
						   PointerGetDatum(noargs),
						   ObjectIdGetDatum(nspid));
}

/*
 * Deferring pre-scan tuple locking requires that no BEFORE STATEMENT trigger
 * could advance snapshot state and no user BEFORE ROW trigger could suppress
 * the modification (returning NULL without incrementing es_processed).
 * Verify extension-owned row triggers strictly by function OID, never by name.
 */
static bool
safe_before_triggers(TriggerDesc *trigdesc, CmdType cmd)
{
	Oid			autoinc_oid;
	Oid			onupd_oid;

	if (trigdesc == NULL)
		return true;
	if (cmd == CMD_DELETE)
		return !trigdesc->trig_delete_before_statement &&
			!trigdesc->trig_delete_before_row;
	if (cmd != CMD_UPDATE)
		return false;
	if (trigdesc->trig_update_before_statement)
		return false;
	if (!trigdesc->trig_update_before_row)
		return true;

	autoinc_oid = lookup_mysql_trigger_fn("auto_increment_advance");
	onupd_oid = lookup_mysql_trigger_fn("on_update_current_timestamp");
	for (int i = 0; i < trigdesc->numtriggers; i++)
	{
		Trigger    *tg = &trigdesc->triggers[i];

		if (tg->tgenabled == TRIGGER_DISABLED)
			continue;
		if (!TRIGGER_TYPE_MATCHES(tg->tgtype, TRIGGER_TYPE_ROW,
								  TRIGGER_TYPE_BEFORE, TRIGGER_TYPE_UPDATE))
			continue;
		if (tg->tgqual != NULL ||
			!OidIsValid(tg->tgfoid) ||
			(tg->tgfoid != autoinc_oid && tg->tgfoid != onupd_oid))
			return false;
	}
	return true;
}

static Node *
current_read_state(CustomScan *plan)
{
	MysCurrentRead *node = palloc0(sizeof(MysCurrentRead));

	NodeSetTag(node, T_CustomScanState);
	node->css.methods = &exec_methods;
	node->css.slotOps = &TTSOpsBufferHeapTuple;
	ItemPointerSetInvalid(&node->deferred_tid);
	return (Node *) node;
}

static void
current_read_begin(CustomScanState *css, EState *estate, int eflags)
{
	MysCurrentRead *node = (MysCurrentRead *) css;
	CustomScan *plan = (CustomScan *) css->ss.ps.plan;
	Index		rti = plan->scan.scanrelid;
	ResultRelInfo *rri;

	if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) || estate->es_epq_active != NULL)
		return;

	rri = estate->es_result_relations[rti - 1];
	node->lockmode = estate->es_plannedstmt->commandType == CMD_UPDATE ?
		ExecUpdateLockMode(estate, rri) : LockTupleExclusive;
	node->index = index_open((Oid) intVal(linitial(plan->custom_private)),
							 exec_rt_fetch(rti, estate)->rellockmode);
	node->current_read = mys_is_mysql_session() && !IsolationUsesXactSnapshot();
	node->can_defer_lock = node->current_read &&
		list_length(plan->custom_private) >= 2 &&
		boolVal(lsecond(plan->custom_private)) &&
		safe_before_triggers(rri->ri_TrigDesc,
							 estate->es_plannedstmt->commandType);
	ItemPointerSetInvalid(&node->deferred_tid);
	ExecIndexBuildScanKeys(&css->ss.ps, node->index, plan->custom_exprs, false,
						   &node->keys, &node->nkeys,
						   &node->runtime_keys, &node->nruntime_keys, NULL, NULL);
	if (node->nruntime_keys > 0)
		node->runtime_context = CreateExprContext(estate);
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

/*
 * True if the buffer heap tuple currently carries neither an update/delete
 * xmax nor a live exclusive/shared tuple lock (non-multi KEY SHARE from child
 * FK checks and already-ended LOCK_ONLY xids/multixacts are allowed).
 */
static bool
tuple_has_no_modify_lock(TupleTableSlot *slot)
{
	BufferHeapTupleTableSlot *bslot = (BufferHeapTupleTableSlot *) slot;
	uint16		infomask;
	TransactionId raw_xmax;

	Assert(slot->tts_ops == &TTSOpsBufferHeapTuple);
	if (!BufferIsValid(bslot->buffer) || bslot->base.tuple == NULL)
		return false;

	LockBuffer(bslot->buffer, BUFFER_LOCK_SHARE);
	infomask = bslot->base.tuple->t_data->t_infomask;
	raw_xmax = HeapTupleHeaderGetRawXmax(bslot->base.tuple->t_data);
	LockBuffer(bslot->buffer, BUFFER_LOCK_UNLOCK);

	if ((infomask & HEAP_XMAX_INVALID) != 0)
		return true;
	if (!HEAP_XMAX_IS_LOCKED_ONLY(infomask))
		return false;
	if ((infomask & HEAP_XMAX_IS_MULTI) == 0)
	{
		if ((infomask & HEAP_LOCK_MASK) == HEAP_XMAX_KEYSHR_LOCK)
			return true;
		return !TransactionIdIsInProgress(raw_xmax);
	}
	return !MultiXactIdIsRunning(raw_xmax, true);
}

static void
open_index_scan(MysCurrentRead *node, ScanState *ss, bool fresh_snapshot)
{
	EState	   *estate = ss->ps.state;

	if (!node->keys_evaluated)
	{
		if (node->nruntime_keys > 0)
		{
			ResetExprContext(node->runtime_context);
			ExecIndexEvalRuntimeKeys(node->runtime_context,
									 node->runtime_keys, node->nruntime_keys);
		}
		node->keys_evaluated = true;
	}
	node->snapshot = RegisterSnapshot(fresh_snapshot ?
									  GetLatestSnapshot() : estate->es_snapshot);
	/* Refresh other transactions, not this command's own visibility. */
	if (fresh_snapshot)
		node->snapshot->curcid = estate->es_snapshot->curcid;
	node->scan = index_beginscan(ss->ss_currentRelation, node->index,
								 node->snapshot, NULL, node->nkeys, 0, SO_NONE);
	index_rescan(node->scan, node->keys, node->nkeys, NULL, 0);
}

static TupleTableSlot *
current_read_next(ScanState *ss)
{
	MysCurrentRead *node = (MysCurrentRead *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (node->done)
		return ExecClearTuple(slot);
	if (node->scan == NULL)
		open_index_scan(node, ss, false);
	if (!index_getnext_slot(node->scan, ForwardScanDirection, slot))
	{
		node->done = true;
		return ExecClearTuple(slot);
	}
	node->done = true;
	return slot;
}

static bool
current_read_recheck(ScanState *ss, TupleTableSlot *slot)
{
	/* The complete key and all remaining quals are in the scan's qual list. */
	return true;
}

static TupleTableSlot *
current_read_exec(CustomScanState *css)
{
	MysCurrentRead *node = (MysCurrentRead *) css;
	ScanState  *ss = &css->ss;
	EState	   *estate = ss->ps.state;
	ExprContext *econtext = ss->ps.ps_ExprContext;
	ExprState  *qual = ss->ps.qual;
	ProjectionInfo *projInfo = ss->ps.ps_ProjInfo;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	/* EPQ and non-current-read modes use standard ExecScan machinery. */
	if (estate->es_epq_active != NULL || !node->current_read)
		return ExecScan(ss, current_read_next, current_read_recheck);

	if (node->deferred_check)
	{
		node->deferred_check = false;
		if (estate->es_processed > node->prev_processed)
		{
			node->done = true;
			return NULL;
		}
		/*
		 * ExecModifyTable skipped our uncontended candidate (e.g. concurrent
		 * DELETE + reinsert or key move raced between scan and heap modify).
		 * Re-scan with a fresh snapshot.
		 */
		close_scan(node);
		node->use_latest_snapshot = true;
	}

	while (!node->done)
	{
		TM_FailureData tmfd;
		TM_Result	result;
		ItemPointerData tid;

		CHECK_FOR_INTERRUPTS();
		if (econtext)
			ResetExprContext(econtext);

		if (node->scan == NULL)
			open_index_scan(node, ss,
							!node->can_defer_lock || node->use_latest_snapshot);

		if (!index_getnext_slot(node->scan, ForwardScanDirection, slot))
			break;

		tid = slot->tts_tid;
		if (node->can_defer_lock &&
			tuple_has_no_modify_lock(slot) &&
			!(ItemPointerIsValid(&node->deferred_tid) &&
			  ItemPointerEquals(&tid, &node->deferred_tid)))
		{
			if (econtext)
				econtext->ecxt_scantuple = slot;
			if (qual == NULL || ExecQual(qual, econtext))
			{
				node->deferred_check = true;
				node->prev_processed = estate->es_processed;
				node->deferred_tid = tid;
				return projInfo ? ExecProject(projInfo) : slot;
			}
			node->done = true;
			ExecClearTuple(slot);
			return NULL;
		}

		/*
		 * Contended or non-deferrable path: lock with ExecUpdateLockMode and
		 * flags = 0 so concurrent key-moving UPDATEs return TM_Updated without
		 * locking the moved tuple on its new key.
		 */
		result = table_tuple_lock(ss->ss_currentRelation, &tid, node->snapshot,
								  slot, estate->es_output_cid,
								  node->lockmode, LockWaitBlock, 0, &tmfd);
		if (result == TM_Ok)
		{
			node->done = true;
			if (econtext)
				econtext->ecxt_scantuple = slot;
			if (qual == NULL || ExecQual(qual, econtext))
				return projInfo ? ExecProject(projInfo) : slot;
			ExecClearTuple(slot);
			return NULL;
		}
		if (result == TM_SelfModified)
			break;
		if (result != TM_Updated && result != TM_Deleted)
			elog(ERROR, "unexpected current-read tuple lock status: %u", result);

		ExecClearTuple(slot);
		close_scan(node);
		node->use_latest_snapshot = true;
	}

	node->done = true;
	ExecClearTuple(slot);
	return NULL;
}

static void
current_read_end(CustomScanState *css)
{
	MysCurrentRead *node = (MysCurrentRead *) css;

	close_scan(node);
	if (node->runtime_context)
	{
		FreeExprContext(node->runtime_context, true);
		node->runtime_context = NULL;
	}
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
	node->deferred_check = false;
	node->use_latest_snapshot = false;
	node->keys_evaluated = false;
	ItemPointerSetInvalid(&node->deferred_tid);
	ExecScanReScan(&css->ss);
}
