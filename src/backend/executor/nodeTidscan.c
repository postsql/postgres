/*-------------------------------------------------------------------------
 *
 * nodeTidscan.c
 *	  Routines to support direct tid scans of relations
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeTidscan.c
 *
 *-------------------------------------------------------------------------
 */
/*
 * INTERFACE ROUTINES
 *
 *		ExecTidScan			scans a relation using tids
 *		ExecInitTidScan		creates and initializes state info.
 *		ExecReScanTidScan	rescans the tid relation.
 *		ExecEndTidScan		releases all storage.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/sysattr.h"
#include "access/tableam.h"
#include "access/tableam_indexscan.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeTidscan.h"
#include "lib/qunique.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/itemid.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


/*
 * It's sufficient to check varattno to identify the CTID variable, as any
 * Var in the relation scan qual must be for our table.  (Even if it's a
 * parameterized scan referencing some other table's CTID, the other table's
 * Var would have become a Param by the time it gets here.)
 */
#define IsCTIDVar(node)  \
	((node) != NULL && \
	 IsA((node), Var) && \
	 ((Var *) (node))->varattno == SelfItemPointerAttributeNumber)

static inline bool
IsRowIdentityVarNode(Node *node, AttrNumber rowid_attnum)
{
	return (rowid_attnum > 0 &&
			node != NULL &&
			IsA(node, Var) &&
			((Var *) node)->varattno == rowid_attnum);
}

/* one element in tss_tidexprs */
typedef struct TidExpr
{
	ExprState  *exprstate;		/* ExprState for a TID-yielding subexpr */
	bool		isarray;		/* if true, it yields tid[] not just tid */
	CurrentOfExpr *cexpr;		/* alternatively, we can have CURRENT OF */
	bool		is_rowidentity; /* if true, expr yields int8/int4/int2 rowid */
	Oid			elemtype;		/* type OID of expression or array element */
} TidExpr;

static void TidExprListCreate(TidScanState *tidstate);
static void TidListEval(TidScanState *tidstate);
static int	itemptr_comparator(const void *a, const void *b);
static TupleTableSlot *TidNext(TidScanState *node);

static inline int64
DatumGetRowId(Datum d, Oid elemtype)
{
	switch (elemtype)
	{
		case INT8OID:
			return DatumGetInt64(d);
		case INT4OID:
			return (int64) DatumGetInt32(d);
		case INT2OID:
			return (int64) DatumGetInt16(d);
		default:
			elog(ERROR, "unsupported row identity comparison type: %u", elemtype);
			return 0;
	}
}

static inline void
TidListAppend(ItemPointerData **tidList, int *numTids, int *numAllocTids,
			  ItemPointer tid)
{
	if (*numTids >= *numAllocTids)
	{
		*numAllocTids = (*numAllocTids > 0) ? (*numAllocTids * 2) : 8;
		*tidList = repalloc_array(*tidList, ItemPointerData, *numAllocTids);
	}
	(*tidList)[(*numTids)++] = *tid;
}

/*
 * Probe the partial B-Tree index (rd_rowidindex) for an off-root tuple with
 * the specified 64-bit rowid, and append any visible tuple's physical TID to
 * *tidList.
 */
static bool
RowIdProbeIndex(Relation rel, Relation *idxrel_p, Snapshot snapshot,
				int64 rowid, ItemPointerData **tidList,
				int *numTids, int *numAllocTids)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			rowid_attno = tupdesc->td_rowidentity_attno;
	Relation	idxrel;
	ScanKeyData skey;
	IndexScanDesc iscan;
	bool		found = false;

	if (rowid_attno < 0)
		return false;

	idxrel = *idxrel_p;
	if (idxrel == NULL)
	{
		Oid			idxoid = RelationGetRowIdIndex(rel);

		if (!OidIsValid(idxoid))
			return false;
		idxrel = index_open(idxoid, AccessShareLock);
		*idxrel_p = idxrel;
	}

	/*
	 * Fast path: if the partial B-Tree index has <= 1 block (metapage only),
	 * zero off-page tuples exist in this relation!
	 */
	if (RelationGetNumberOfBlocks(idxrel) <= 1)
		return false;

	ScanKeyInit(&skey,
				1,
				BTEqualStrategyNumber,
				F_INT8EQ,
				Int64GetDatum(rowid));

	iscan = index_beginscan(rel, idxrel, false, snapshot, NULL, 1, 0, SO_NONE);
	index_rescan(iscan, &skey, 1, NULL, 0);

	while (tableam_index_getnext_tid(iscan, ForwardScanDirection))
	{
		ItemPointerData heaptid = iscan->xs_heaptid;
		Buffer		buf;
		HeapTupleData heapTuple;
		bool		all_dead = false;

		buf = ReadBuffer(rel, ItemPointerGetBlockNumber(&heaptid));
		LockBuffer(buf, BUFFER_LOCK_SHARE);

		if (heap_hot_search_buffer(&heaptid, rel, buf, snapshot,
								   &heapTuple, &all_dead, true))
		{
			bool		isnull = false;
			Datum		val;

			/*
			 * Verify the visible tuple in this HOT chain is indeed an
			 * off-root tuple carrying the target rowid (and not a home tuple
			 * after the slot was recycled).
			 */
			if (!HeapTupleHeaderRowIdIsAtHome(heapTuple.t_data, rowid_attno))
			{
				val = fastgetattr(&heapTuple, rowid_attno + 1, tupdesc, &isnull);
				if (!isnull && DatumGetInt64(val) == rowid)
				{
					TidListAppend(tidList, numTids, numAllocTids, &heapTuple.t_self);
					found = true;
				}
			}
		}

		UnlockReleaseBuffer(buf);
	}

	index_endscan(iscan);
	return found;
}

/*
 * Resolve a 64-bit row identity key to the physical TID(s) of any matching
 * visible tuple in 'rel', using the two-mode adaptive lookup strategy:
 *   - Mode 1 (Heap-First): Pin B_0 and walk the HOT chain at O_0. If O_0 is
 *     LP_UNUSED or a dead local DELETE chain, return 0 rows in 0 index reads.
 *     Only probe rd_rowidindex when O_0 is LP_REDIRECT_OFFPAGE_* or a chain
 *     that moved off-root / was repatriated after our snapshot.
 *   - Mode 2 (Cached-Tiny-Index-First when B_0 is cold): When rd_rowidindex
 *     is non-empty and tiny (<= 64 blocks) and PrefetchBuffer reports B_0 is
 *     not in shared_buffers, probe rd_rowidindex while B_0 is prefetched.
 */
static void
RowIdResolveToTid(Relation rel, Relation *idxrel_p, BlockNumber nblocks,
				  Snapshot snapshot, int64 rowid,
				  ItemPointerData **tidList, int *numTids, int *numAllocTids)
{
	TupleDesc	tupdesc = RelationGetDescr(rel);
	int			rowid_attno = tupdesc->td_rowidentity_attno;
	BlockNumber blkno;
	OffsetNumber rootoff;
	Buffer		buf;
	Page		page;
	OffsetNumber maxoff;
	ItemId		rootlp;
	OffsetNumber curoff;
	TransactionId prev_xmax;
	bool		need_index_probe = false;
	bool		already_probed_index = false;

	if (rowid <= 0 || rowid_attno < 0)
		return;

	/*
	 * Extended keys (bit 62 set) or keys belonging to another partition
	 * cannot reside at a local home TID; probe the partial index directly.
	 */
	if (RowIdIsExtended(rowid) ||
		RowIdGetPartitionId(rowid, tupdesc->td_partition_bits) != tupdesc->td_partition_prefix)
	{
		(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
							   tidList, numTids, numAllocTids);
		return;
	}

	blkno = RowIdGetBlockNumber(rowid, tupdesc->td_partition_bits);
	rootoff = RowIdGetOffsetNumber(rowid);

	if (rootoff < FirstOffsetNumber || rootoff > MaxHeapTuplesPerPage)
	{
		(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
							   tidList, numTids, numAllocTids);
		return;
	}

	if (blkno >= nblocks)
	{
		(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
							   tidList, numTids, numAllocTids);
		return;
	}

	/*
	 * Mode 2 check: if rd_rowidindex is non-empty and tiny (2..64 blocks),
	 * check if B_0 is in shared_buffers via PrefetchBuffer.
	 */
	if (*idxrel_p == NULL && OidIsValid(RelationGetRowIdIndex(rel)))
		*idxrel_p = index_open(RelationGetRowIdIndex(rel), AccessShareLock);

	if (*idxrel_p != NULL)
	{
		BlockNumber idx_nblocks = RelationGetNumberOfBlocks(*idxrel_p);

		if (idx_nblocks > 1 && idx_nblocks <= 64)
		{
			PrefetchBufferResult pf = PrefetchBuffer(rel, MAIN_FORKNUM, blkno);

			if (!BufferIsValid(pf.recent_buffer))
			{
				if (RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
									tidList, numTids, numAllocTids))
					return;
				already_probed_index = true;
			}
		}
	}

	/*
	 * Mode 1: Pin home block B_0 and inspect line pointer O_0.
	 */
	buf = ReadBuffer(rel, blkno);
	LockBuffer(buf, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buf);
	maxoff = PageGetMaxOffsetNumber(page);

	if (rootoff > maxoff)
	{
		UnlockReleaseBuffer(buf);
		if (!already_probed_index)
			(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
								   tidList, numTids, numAllocTids);
		return;
	}

	rootlp = PageGetItemId(page, rootoff);

	/*
	 * If O_0 is LP_UNUSED or LP_DEAD, check RowIdProbeIndex in case the row
	 * was moved off-home by CLUSTER / VACUUM FULL or inserted via OVERRIDING
	 * SYSTEM VALUE without anchoring O_0 (RowIdProbeIndex is a 0-I/O no-op
	 * when rd_rowidindex has <= 1 block).
	 */
	if (!ItemIdIsUsed(rootlp) || ItemIdIsDead(rootlp))
	{
		UnlockReleaseBuffer(buf);
		if (!already_probed_index)
			(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
								   tidList, numTids, numAllocTids);
		return;
	}

	if (ItemIdIsRedirected(rootlp))
	{
		OffsetNumber redir = ItemIdGetRedirect(rootlp);

		if (OffsetNumberIsOffpageRedirect(redir))
		{
			UnlockReleaseBuffer(buf);
			if (redir != LP_REDIRECT_OFFPAGE_PART && !already_probed_index)
				(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
									   tidList, numTids, numAllocTids);
			return;
		}
		curoff = redir;
	}
	else
	{
		curoff = rootoff;
	}

	/*
	 * Walk the local HOT chain rooted at rootoff on page B_0.
	 */
	prev_xmax = InvalidTransactionId;
	for (;;)
	{
		ItemId		lp;
		HeapTupleData htup;
		bool		valid;

		if (curoff < FirstOffsetNumber || curoff > maxoff)
			break;

		lp = PageGetItemId(page, curoff);
		if (!ItemIdIsNormal(lp))
			break;

		htup.t_data = (HeapTupleHeader) PageGetItem(page, lp);
		htup.t_len = ItemIdGetLength(lp);
		htup.t_tableOid = RelationGetRelid(rel);
		ItemPointerSet(&htup.t_self, blkno, curoff);
		htup.t_rootoff = rootoff;

		if (prev_xmax != InvalidTransactionId &&
			!TransactionIdEquals(prev_xmax, HeapTupleHeaderGetXmin(htup.t_data)))
			break;

		if (!HeapTupleHeaderRowIdIsAtHome(htup.t_data, rowid_attno))
		{
			need_index_probe = true;
			break;
		}

		valid = HeapTupleSatisfiesVisibility(&htup, snapshot, buf);
		if (valid)
		{
			TidListAppend(tidList, numTids, numAllocTids, &htup.t_self);
			UnlockReleaseBuffer(buf);
			return;
		}

		if ((htup.t_data->t_infomask & HEAP_UPDATED) != 0)
			need_index_probe = true;

		if (HeapTupleHeaderIsHotUpdated(htup.t_data))
		{
			curoff = ItemPointerGetOffsetNumber(&htup.t_data->t_ctid);
			prev_xmax = HeapTupleHeaderGetUpdateXid(htup.t_data);
			continue;
		}

		if (!ItemPointerEquals(&htup.t_self, &htup.t_data->t_ctid) &&
			!HeapTupleHeaderIndicatesMovedPartitions(htup.t_data))
		{
			need_index_probe = true;
		}
		break;
	}

	UnlockReleaseBuffer(buf);

	if (need_index_probe && !already_probed_index)
		(void) RowIdProbeIndex(rel, idxrel_p, snapshot, rowid,
							   tidList, numTids, numAllocTids);
}


/*
 * Extract the qual subexpressions that yield TIDs to search for,
 * and compile them into ExprStates if they're ordinary expressions.
 *
 * CURRENT OF is a special case that we can't compile usefully;
 * just drop it into the TidExpr list as-is.
 */
static void
TidExprListCreate(TidScanState *tidstate)
{
	TidScan    *node = (TidScan *) tidstate->ss.ps.plan;
	Relation	rel = tidstate->ss.ss_currentRelation;
	AttrNumber	rowid_attnum = RelationGetDescr(rel)->td_rowidentity_attno + 1;
	ListCell   *l;

	tidstate->tss_tidexprs = NIL;
	tidstate->tss_isCurrentOf = false;

	foreach(l, node->tidquals)
	{
		Expr	   *expr = (Expr *) lfirst(l);
		TidExpr    *tidexpr = palloc0_object(TidExpr);

		if (is_opclause(expr))
		{
			Node	   *arg1;
			Node	   *arg2;

			arg1 = get_leftop(expr);
			arg2 = get_rightop(expr);
			if (IsCTIDVar(arg1))
			{
				tidexpr->exprstate = ExecInitExpr((Expr *) arg2,
												  &tidstate->ss.ps);
				tidexpr->is_rowidentity = false;
			}
			else if (IsCTIDVar(arg2))
			{
				tidexpr->exprstate = ExecInitExpr((Expr *) arg1,
												  &tidstate->ss.ps);
				tidexpr->is_rowidentity = false;
			}
			else if (IsRowIdentityVarNode(arg1, rowid_attnum))
			{
				tidexpr->exprstate = ExecInitExpr((Expr *) arg2,
												  &tidstate->ss.ps);
				tidexpr->is_rowidentity = true;
				tidexpr->elemtype = exprType(arg2);
			}
			else if (IsRowIdentityVarNode(arg2, rowid_attnum))
			{
				tidexpr->exprstate = ExecInitExpr((Expr *) arg1,
												  &tidstate->ss.ps);
				tidexpr->is_rowidentity = true;
				tidexpr->elemtype = exprType(arg1);
			}
			else
				elog(ERROR, "could not identify CTID variable");
			tidexpr->isarray = false;
		}
		else if (expr && IsA(expr, ScalarArrayOpExpr))
		{
			ScalarArrayOpExpr *saex = (ScalarArrayOpExpr *) expr;
			Node	   *arg1 = linitial(saex->args);
			Expr	   *arg2 = lsecond(saex->args);

			if (IsCTIDVar(arg1))
			{
				tidexpr->is_rowidentity = false;
				tidexpr->elemtype = TIDOID;
			}
			else if (IsRowIdentityVarNode(arg1, rowid_attnum))
			{
				tidexpr->is_rowidentity = true;
				tidexpr->elemtype = get_element_type(exprType((Node *) arg2));
			}
			else
				elog(ERROR, "could not identify CTID variable");

			tidexpr->exprstate = ExecInitExpr(arg2, &tidstate->ss.ps);
			tidexpr->isarray = true;
		}
		else if (expr && IsA(expr, CurrentOfExpr))
		{
			CurrentOfExpr *cexpr = (CurrentOfExpr *) expr;

			tidexpr->cexpr = cexpr;
			tidstate->tss_isCurrentOf = true;
		}
		else
			elog(ERROR, "could not identify CTID expression");

		tidstate->tss_tidexprs = lappend(tidstate->tss_tidexprs, tidexpr);
	}

	/* CurrentOfExpr could never appear OR'd with something else */
	Assert(list_length(tidstate->tss_tidexprs) == 1 ||
		   !tidstate->tss_isCurrentOf);
}

/*
 * Compute the list of TIDs to be visited, by evaluating the expressions
 * for them.
 *
 * (The result is actually an array, not a list.)
 */
static void
TidListEval(TidScanState *tidstate)
{
	ExprContext *econtext = tidstate->ss.ps.ps_ExprContext;
	Relation	rel = tidstate->ss.ss_currentRelation;
	Snapshot	snapshot = tidstate->ss.ps.state->es_snapshot;
	TableScanDesc scan;
	Relation	rowid_idxrel = NULL;
	BlockNumber nblocks = InvalidBlockNumber;
	ItemPointerData *tidList;
	int			numAllocTids;
	int			numTids;
	ListCell   *l;

	/*
	 * Start scan on-demand - initializing a scan isn't free (e.g. heap stats
	 * the size of the table), so it makes sense to delay that until needed -
	 * the node might never get executed.
	 */
	if (tidstate->ss.ss_currentScanDesc == NULL)
		tidstate->ss.ss_currentScanDesc =
			table_beginscan_tid(rel, snapshot);
	scan = tidstate->ss.ss_currentScanDesc;

	/*
	 * We initialize the array with enough slots for the case that all quals
	 * are simple OpExprs or CurrentOfExprs.  If there are any
	 * ScalarArrayOpExprs, we may have to enlarge the array.
	 */
	numAllocTids = list_length(tidstate->tss_tidexprs);
	tidList = palloc_array(ItemPointerData, numAllocTids);
	numTids = 0;

	foreach(l, tidstate->tss_tidexprs)
	{
		TidExpr    *tidexpr = (TidExpr *) lfirst(l);
		ItemPointer itemptr;
		bool		isNull;

		if (tidexpr->is_rowidentity)
		{
			if (nblocks == InvalidBlockNumber)
				nblocks = RelationGetNumberOfBlocks(rel);

			if (tidexpr->exprstate && !tidexpr->isarray)
			{
				Datum		d;

				d = ExecEvalExprSwitchContext(tidexpr->exprstate,
											  econtext,
											  &isNull);
				if (isNull)
					continue;

				RowIdResolveToTid(rel, &rowid_idxrel, nblocks, snapshot,
								  DatumGetRowId(d, tidexpr->elemtype),
								  &tidList, &numTids, &numAllocTids);
			}
			else if (tidexpr->exprstate && tidexpr->isarray)
			{
				Datum		arraydatum;
				ArrayType  *itemarray;
				Datum	   *ipdatums;
				bool	   *ipnulls;
				int			ndatums;
				int			i;
				TupleDesc	tupdesc = RelationGetDescr(rel);

				arraydatum = ExecEvalExprSwitchContext(tidexpr->exprstate,
													   econtext,
													   &isNull);
				if (isNull)
					continue;
				itemarray = DatumGetArrayTypeP(arraydatum);
				{
					int16		elmlen = (tidexpr->elemtype == INT8OID) ? 8 :
						((tidexpr->elemtype == INT4OID) ? 4 : 2);
					char		elmalign = (tidexpr->elemtype == INT8OID) ? TYPALIGN_DOUBLE :
						((tidexpr->elemtype == INT4OID) ? TYPALIGN_INT : TYPALIGN_SHORT);

					deconstruct_array(itemarray, tidexpr->elemtype,
									  elmlen, true, elmalign,
									  &ipdatums, &ipnulls, &ndatums);
				}

				/* Issue async prefetches for batch array lookups */
				if (ndatums > 1)
				{
					for (i = 0; i < ndatums; i++)
					{
						int64		rowid;

						if (ipnulls[i])
							continue;
						rowid = DatumGetRowId(ipdatums[i], tidexpr->elemtype);
						if (rowid > 0 && !RowIdIsExtended(rowid) &&
							RowIdGetPartitionId(rowid, tupdesc->td_partition_bits) == tupdesc->td_partition_prefix)
						{
							BlockNumber blkno = RowIdGetBlockNumber(rowid, tupdesc->td_partition_bits);

							if (blkno < nblocks)
								(void) PrefetchBuffer(rel, MAIN_FORKNUM, blkno);
						}
					}
				}

				for (i = 0; i < ndatums; i++)
				{
					if (ipnulls[i])
						continue;

					RowIdResolveToTid(rel, &rowid_idxrel, nblocks, snapshot,
									  DatumGetRowId(ipdatums[i], tidexpr->elemtype),
									  &tidList, &numTids, &numAllocTids);
				}
				pfree(ipdatums);
				pfree(ipnulls);
			}
			continue;
		}

		if (tidexpr->exprstate && !tidexpr->isarray)
		{
			itemptr = (ItemPointer)
				DatumGetPointer(ExecEvalExprSwitchContext(tidexpr->exprstate,
														  econtext,
														  &isNull));
			if (isNull)
				continue;

			/*
			 * We silently discard any TIDs that the AM considers invalid
			 * (E.g. for heap, they could be out of range at the time of scan
			 * start.  Since we hold at least AccessShareLock on the table, it
			 * won't be possible for someone to truncate away the blocks we
			 * intend to visit.).
			 */
			if (!table_tuple_tid_valid(scan, itemptr))
				continue;

			if (numTids >= numAllocTids)
			{
				numAllocTids *= 2;
				tidList = repalloc_array(tidList, ItemPointerData, numAllocTids);
			}
			tidList[numTids++] = *itemptr;
		}
		else if (tidexpr->exprstate && tidexpr->isarray)
		{
			Datum		arraydatum;
			ArrayType  *itemarray;
			Datum	   *ipdatums;
			bool	   *ipnulls;
			int			ndatums;
			int			i;

			arraydatum = ExecEvalExprSwitchContext(tidexpr->exprstate,
												   econtext,
												   &isNull);
			if (isNull)
				continue;
			itemarray = DatumGetArrayTypeP(arraydatum);
			deconstruct_array_builtin(itemarray, TIDOID, &ipdatums, &ipnulls, &ndatums);
			if (numTids + ndatums > numAllocTids)
			{
				numAllocTids = numTids + ndatums;
				tidList = repalloc_array(tidList, ItemPointerData, numAllocTids);
			}
			for (i = 0; i < ndatums; i++)
			{
				if (ipnulls[i])
					continue;

				itemptr = (ItemPointer) DatumGetPointer(ipdatums[i]);

				if (!table_tuple_tid_valid(scan, itemptr))
					continue;

				tidList[numTids++] = *itemptr;
			}
			pfree(ipdatums);
			pfree(ipnulls);
		}
		else
		{
			ItemPointerData cursor_tid;

			Assert(tidexpr->cexpr);
			if (execCurrentOf(tidexpr->cexpr, econtext,
							  RelationGetRelid(tidstate->ss.ss_currentRelation),
							  &cursor_tid))
			{
				if (numTids >= numAllocTids)
				{
					numAllocTids *= 2;
					tidList = repalloc_array(tidList, ItemPointerData, numAllocTids);
				}
				tidList[numTids++] = cursor_tid;
			}
		}
	}

	if (rowid_idxrel != NULL)
		index_close(rowid_idxrel, AccessShareLock);

	/*
	 * Sort the array of TIDs into order, and eliminate duplicates.
	 * Eliminating duplicates is necessary since we want OR semantics across
	 * the list.  Sorting makes it easier to detect duplicates, and as a bonus
	 * ensures that we will visit the heap in the most efficient way.
	 */
	if (numTids > 1)
	{
		/* CurrentOfExpr could never appear OR'd with something else */
		Assert(!tidstate->tss_isCurrentOf);

		qsort(tidList, numTids, sizeof(ItemPointerData),
			  itemptr_comparator);
		numTids = qunique(tidList, numTids, sizeof(ItemPointerData),
						  itemptr_comparator);
	}

	tidstate->tss_TidList = tidList;
	tidstate->tss_NumTids = numTids;
	tidstate->tss_TidPtr = -1;
}

/*
 * qsort comparator for ItemPointerData items
 */
static int
itemptr_comparator(const void *a, const void *b)
{
	const ItemPointerData *ipa = (const ItemPointerData *) a;
	const ItemPointerData *ipb = (const ItemPointerData *) b;
	BlockNumber ba = ItemPointerGetBlockNumber(ipa);
	BlockNumber bb = ItemPointerGetBlockNumber(ipb);
	OffsetNumber oa = ItemPointerGetOffsetNumber(ipa);
	OffsetNumber ob = ItemPointerGetOffsetNumber(ipb);

	if (ba < bb)
		return -1;
	if (ba > bb)
		return 1;
	if (oa < ob)
		return -1;
	if (oa > ob)
		return 1;
	return 0;
}

/* ----------------------------------------------------------------
 *		TidNext
 *
 *		Retrieve a tuple from the TidScan node's currentRelation
 *		using the tids in the TidScanState information.
 *
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
TidNext(TidScanState *node)
{
	EState	   *estate;
	ScanDirection direction;
	Snapshot	snapshot;
	TableScanDesc scan;
	Relation	heapRelation;
	TupleTableSlot *slot;
	ItemPointerData *tidList;
	int			numTids;
	bool		bBackward;

	/*
	 * extract necessary information from tid scan node
	 */
	estate = node->ss.ps.state;
	direction = estate->es_direction;
	snapshot = estate->es_snapshot;
	heapRelation = node->ss.ss_currentRelation;
	slot = node->ss.ss_ScanTupleSlot;

	/*
	 * First time through, compute the list of TIDs to be visited
	 */
	if (node->tss_TidList == NULL)
		TidListEval(node);

	scan = node->ss.ss_currentScanDesc;
	tidList = node->tss_TidList;
	numTids = node->tss_NumTids;

	/*
	 * Initialize or advance scan position, depending on direction.
	 */
	bBackward = ScanDirectionIsBackward(direction);
	if (bBackward)
	{
		if (node->tss_TidPtr < 0)
		{
			/* initialize for backward scan */
			node->tss_TidPtr = numTids - 1;
		}
		else
			node->tss_TidPtr--;
	}
	else
	{
		if (node->tss_TidPtr < 0)
		{
			/* initialize for forward scan */
			node->tss_TidPtr = 0;
		}
		else
			node->tss_TidPtr++;
	}

	while (node->tss_TidPtr >= 0 && node->tss_TidPtr < numTids)
	{
		ItemPointerData tid = tidList[node->tss_TidPtr];

		/*
		 * For WHERE CURRENT OF, the tuple retrieved from the cursor might
		 * since have been updated; if so, we should fetch the version that is
		 * current according to our snapshot.
		 */
		if (node->tss_isCurrentOf)
			table_tuple_get_latest_tid(scan, &tid);

		if (table_tuple_fetch_row_version(heapRelation, &tid, snapshot, slot))
			return slot;

		/* Bad TID or failed snapshot qual; try next */
		if (bBackward)
			node->tss_TidPtr--;
		else
			node->tss_TidPtr++;

		CHECK_FOR_INTERRUPTS();
	}

	/*
	 * if we get here it means the tid scan failed so we are at the end of the
	 * scan..
	 */
	return ExecClearTuple(slot);
}

/*
 * TidRecheck -- access method routine to recheck a tuple in EvalPlanQual
 */
static bool
TidRecheck(TidScanState *node, TupleTableSlot *slot)
{
	ItemPointer match;
	ListCell   *l;

	/* WHERE CURRENT OF always intends to resolve to the latest tuple */
	if (node->tss_isCurrentOf)
		return true;

	/*
	 * A GENERATED ALWAYS AS ROW IDENTITY value is immutable across UPDATEs,
	 * so the updated version of a qualifying tuple in EvalPlanQual still has
	 * the same row identity even though its physical ctid changed.
	 */
	foreach(l, node->tss_tidexprs)
	{
		TidExpr    *tidexpr = (TidExpr *) lfirst(l);

		if (tidexpr->is_rowidentity)
			return true;
	}

	if (node->tss_TidList == NULL)
		TidListEval(node);

	/*
	 * Binary search the TidList to see if this ctid is mentioned and return
	 * true if it is.
	 */
	match = (ItemPointer) bsearch(&slot->tts_tid, node->tss_TidList,
								  node->tss_NumTids, sizeof(ItemPointerData),
								  itemptr_comparator);
	return match != NULL;
}


/* ----------------------------------------------------------------
 *		ExecTidScan(node)
 *
 *		Scans the relation using tids and returns
 *		   the next qualifying tuple in the direction specified.
 *		We call the ExecScan() routine and pass it the appropriate
 *		access method functions.
 *
 *		Conditions:
 *		  -- the "cursor" maintained by the AMI is positioned at the tuple
 *			 returned previously.
 *
 *		Initial States:
 *		  -- the relation indicated is opened for scanning so that the
 *			 "cursor" is positioned before the first qualifying tuple.
 *		  -- tss_TidPtr is -1.
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ExecTidScan(PlanState *pstate)
{
	TidScanState *node = castNode(TidScanState, pstate);

	return ExecScan(&node->ss,
					(ExecScanAccessMtd) TidNext,
					(ExecScanRecheckMtd) TidRecheck);
}

/* ----------------------------------------------------------------
 *		ExecReScanTidScan(node)
 * ----------------------------------------------------------------
 */
void
ExecReScanTidScan(TidScanState *node)
{
	if (node->tss_TidList)
		pfree(node->tss_TidList);
	node->tss_TidList = NULL;
	node->tss_NumTids = 0;
	node->tss_TidPtr = -1;

	/* not really necessary, but seems good form */
	if (node->ss.ss_currentScanDesc)
		table_rescan(node->ss.ss_currentScanDesc, NULL);

	ExecScanReScan(&node->ss);
}

/* ----------------------------------------------------------------
 *		ExecEndTidScan
 *
 *		Releases any storage allocated through C routines.
 *		Returns nothing.
 * ----------------------------------------------------------------
 */
void
ExecEndTidScan(TidScanState *node)
{
	if (node->ss.ss_currentScanDesc)
		table_endscan(node->ss.ss_currentScanDesc);
}

/* ----------------------------------------------------------------
 *		ExecInitTidScan
 *
 *		Initializes the tid scan's state information, creates
 *		scan keys, and opens the base and tid relations.
 *
 *		Parameters:
 *		  node: TidScan node produced by the planner.
 *		  estate: the execution state initialized in InitPlan.
 * ----------------------------------------------------------------
 */
TidScanState *
ExecInitTidScan(TidScan *node, EState *estate, int eflags)
{
	TidScanState *tidstate;
	Relation	currentRelation;

	/*
	 * create state structure
	 */
	tidstate = makeNode(TidScanState);
	tidstate->ss.ps.plan = (Plan *) node;
	tidstate->ss.ps.state = estate;
	tidstate->ss.ps.ExecProcNode = ExecTidScan;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &tidstate->ss.ps);

	/*
	 * mark tid list as not computed yet
	 */
	tidstate->tss_TidList = NULL;
	tidstate->tss_NumTids = 0;
	tidstate->tss_TidPtr = -1;

	/*
	 * open the scan relation
	 */
	currentRelation = ExecOpenScanRelation(estate, node->scan.scanrelid, eflags);

	tidstate->ss.ss_currentRelation = currentRelation;
	tidstate->ss.ss_currentScanDesc = NULL; /* no heap scan here */

	/*
	 * get the scan type from the relation descriptor.
	 */
	ExecInitScanTupleSlot(estate, &tidstate->ss,
						  RelationGetDescr(currentRelation),
						  table_slot_callbacks(currentRelation),
						  TTS_FLAG_OBEYS_NOT_NULL_CONSTRAINTS);

	/*
	 * Initialize result type and projection.
	 */
	ExecInitResultTypeTL(&tidstate->ss.ps);
	ExecAssignScanProjectionInfo(&tidstate->ss);

	/*
	 * initialize child expressions
	 */
	tidstate->ss.ps.qual =
		ExecInitQual(node->scan.plan.qual, (PlanState *) tidstate);

	TidExprListCreate(tidstate);

	/*
	 * all done.
	 */
	return tidstate;
}
