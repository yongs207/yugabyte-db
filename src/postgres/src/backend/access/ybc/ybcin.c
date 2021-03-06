/*--------------------------------------------------------------------------------------------------
 *
 * ybcin.c
 *	  Implementation of YugaByte indexes.
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * src/backend/access/ybc/ybcin.c
 *
 * TODO: currently this file contains skeleton index access methods. They will be implemented in
 * coming revisions.
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/relscan.h"
#include "access/sysattr.h"
#include "access/ybcam.h"
#include "access/ybcin.h"
#include "catalog/index.h"
#include "catalog/pg_type.h"
#include "utils/rel.h"
#include "executor/ybcModifyTable.h"

/* --------------------------------------------------------------------------------------------- */

/* Working state for ybcinbuild and its callback */
typedef struct
{
	double	  index_tuples;
} YBCBuildState;

static void
ybcinbuildCallback(Relation index, HeapTuple heapTuple, Datum *values, bool *isnull,
				   bool tupleIsAlive, void *state)
{
	YBCBuildState  *buildstate = (YBCBuildState *)state;

	YBCExecuteInsertIndex(index, values, isnull, heapTuple->t_ybctid);
	buildstate->index_tuples += 1;
}

IndexBuildResult *
ybcinbuild(Relation heap, Relation index, struct IndexInfo *indexInfo)
{
	YBCBuildState	buildstate;
	double			heap_tuples = 0;

	Assert(!index->rd_index->indisprimary);

	/* Do the heap scan */
	buildstate.index_tuples = 0;
	heap_tuples = IndexBuildHeapScan(heap, index, indexInfo, true, ybcinbuildCallback,
									 &buildstate);

	/*
	 * Return statistics
	 */
	IndexBuildResult *result = (IndexBuildResult *) palloc(sizeof(IndexBuildResult));
	result->heap_tuples  = heap_tuples;
	result->index_tuples = buildstate.index_tuples;
	return result;
}

void
ybcinbuildempty(Relation index)
{
	YBC_LOG_WARNING("Unexpected building of empty unlogged index");
}

bool
ybcininsert(Relation index, Datum *values, bool *isnull, Datum ybctid, Relation heap,
			IndexUniqueCheck checkUnique, struct IndexInfo *indexInfo)
{
	Assert(!index->rd_index->indisprimary);

	YBCExecuteInsertIndex(index, values, isnull, ybctid);
	
	return index->rd_index->indisunique ? true : false;
}

void
ybcindelete(Relation index, Datum *values, bool *isnull, Datum ybctid, Relation heap,
			struct IndexInfo *indexInfo)
{
	YBCExecuteDeleteIndex(index, values, isnull, ybctid);
}

IndexBulkDeleteResult *
ybcinbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				IndexBulkDeleteCallback callback, void *callback_state)
{
	YBC_LOG_WARNING("Unexpected bulk delete of index via vacuum");
	return NULL;
}

IndexBulkDeleteResult *
ybcinvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	YBC_LOG_WARNING("Unexpected index cleanup via vacuum");
	return NULL;
}

/* --------------------------------------------------------------------------------------------- */

bool ybcincanreturn(Relation index, int attno)
{
	return false;
}

void
ybcincostestimate(struct PlannerInfo *root, struct IndexPath *path, double loop_count,
				  Cost *indexStartupCost, Cost *indexTotalCost, Selectivity *indexSelectivity,
				  double *indexCorrelation, double *indexPages)
{
}

bytea *
ybcinoptions(Datum reloptions, bool validate)
{
	return NULL;
}

bool
ybcinproperty(Oid index_oid, int attno, IndexAMProperty prop, const char *propname,
			  bool *res, bool *isnull)
{
	return false;	
}

bool
ybcinvalidate(Oid opclassoid)
{
	return true;
}

/* --------------------------------------------------------------------------------------------- */

IndexScanDesc
ybcinbeginscan(Relation rel, int nkeys, int norderbys)
{
	IndexScanDesc scan;

	/* no order by operators allowed */
	Assert(norderbys == 0);

	/* get the scan */
	scan = RelationGetIndexScan(rel, nkeys, norderbys);
	scan->opaque = NULL;

	return scan;
}

void 
ybcinrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,	ScanKey orderbys, int norderbys)
{
	ybc_index_beginscan(scan->indexRelation, scan, nscankeys, scankey);
}

bool
ybcingettuple(IndexScanDesc scan, ScanDirection dir)
{
	HeapTuple tuple = ybc_index_getnext(scan);
	scan->xs_ctup.t_ybctid = (tuple != NULL) ? tuple->t_ybctid : 0;
	return scan->xs_ctup.t_ybctid != 0;
}

int64
ybcingetbitmap(IndexScanDesc scan, TIDBitmap *tbm)
{
	return 0;
}

void 
ybcinendscan(IndexScanDesc scan)
{
	ybc_index_endscan(scan);
}

/* --------------------------------------------------------------------------------------------- */

void 
ybcinmarkpos(IndexScanDesc scan)
{
}

void 
ybcinrestrpos(IndexScanDesc scan)
{
}

/* --------------------------------------------------------------------------------------------- */

HeapTuple
YBCIndexExecuteSelect(Relation relation, Datum ybctid)
{
	YBCPgStatement ybc_stmt;
	TupleDesc      tupdesc = RelationGetDescr(relation);

	HandleYBStatus(YBCPgNewSelect(ybc_pg_session,
								  YBCGetDatabaseOid(relation),
								  RelationGetRelid(relation),
								  InvalidOid,
								  &ybc_stmt,
								  NULL /* read_time */));

	/* Bind ybctid to identify the current row. */
	YBCPgExpr ybctid_expr = YBCNewConstant(ybc_stmt,
										   BYTEAOID,
										   ybctid,
										   false);
	HandleYBStmtStatus(YBCPgDmlBindColumn(ybc_stmt,
										  YBTupleIdAttributeNumber,
										  ybctid_expr), ybc_stmt);

	/*
	 * Set up the scan targets. For index-based scan we need to return all "real" columns.
	 */
	if (RelationGetForm(relation)->relhasoids)
	{
		YBCPgTypeAttrs type_attrs = { 0 };
		YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, ObjectIdAttributeNumber, InvalidOid,
										   &type_attrs);
		HandleYBStmtStatus(YBCPgDmlAppendTarget(ybc_stmt, expr), ybc_stmt);
	}
	for (AttrNumber attnum = 1; attnum <= tupdesc->natts; attnum++)
	{
		Form_pg_attribute att = TupleDescAttr(tupdesc, attnum - 1);
		YBCPgTypeAttrs type_attrs = { att->atttypmod };
		YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, attnum, att->atttypid, &type_attrs);
		HandleYBStmtStatus(YBCPgDmlAppendTarget(ybc_stmt, expr), ybc_stmt);
	}
	YBCPgTypeAttrs type_attrs = { 0 };
	YBCPgExpr   expr = YBCNewColumnRef(ybc_stmt, YBTupleIdAttributeNumber, InvalidOid,
									   &type_attrs);
	HandleYBStmtStatus(YBCPgDmlAppendTarget(ybc_stmt, expr), ybc_stmt);

	/* Execute the select statement. */
	HandleYBStmtStatus(YBCPgExecSelect(ybc_stmt), ybc_stmt);

	HeapTuple tuple    = NULL;
	bool      has_data = false;

	Datum           *values = (Datum *) palloc0(tupdesc->natts * sizeof(Datum));
	bool            *nulls  = (bool *) palloc(tupdesc->natts * sizeof(bool));
	YBCPgSysColumns syscols;

	/* Fetch one row. */
	HandleYBStmtStatus(YBCPgDmlFetch(ybc_stmt,
									 tupdesc->natts,
									 (uint64_t *) values,
									 nulls,
									 &syscols,
									 &has_data),
					   ybc_stmt);

	if (has_data)
	{
		tuple = heap_form_tuple(tupdesc, values, nulls);

		if (syscols.oid != InvalidOid)
		{
			HeapTupleSetOid(tuple, syscols.oid);
		}
		if (syscols.ybctid != NULL)
		{
			tuple->t_ybctid = PointerGetDatum(syscols.ybctid);
		}
	}
	pfree(values);
	pfree(nulls);

	/* Complete execution */
	HandleYBStatus(YBCPgDeleteStatement(ybc_stmt));

	return tuple;
}
