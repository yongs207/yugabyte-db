//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//--------------------------------------------------------------------------------------------------

#include "yb/yql/pggate/pg_dml.h"
#include "yb/yql/pggate/util/pg_doc_data.h"
#include "yb/client/yb_op.h"

namespace yb {
namespace pggate {

using namespace std::literals;  // NOLINT

using client::YBClient;
using client::YBSession;
using client::YBMetaDataCache;
using client::YBTable;
using client::YBTableName;
using client::YBTableType;
using client::YBPgsqlWriteOp;

// TODO(neil) This should be derived from a GFLAGS.
static MonoDelta kSessionTimeout = 60s;

//--------------------------------------------------------------------------------------------------
// PgDml
//--------------------------------------------------------------------------------------------------
PgDml::PgDml(PgSession::ScopedRefPtr pg_session, const PgObjectId& table_id)
    : PgStatement(std::move(pg_session)), table_id_(table_id) {
}

PgDml::~PgDml() {
}

Status PgDml::LoadTable() {
  table_desc_ = VERIFY_RESULT(pg_session_->LoadTable(table_id_));
  return Status::OK();
}

Status PgDml::ClearBinds() {
  return STATUS(NotSupported, "Clearing binds for prepared statement is not yet implemented");
}

Status PgDml::FindColumn(int attr_num, PgColumn **col) {
  *col = VERIFY_RESULT(table_desc_->FindColumn(attr_num));
  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgDml::AppendTarget(PgExpr *target) {
  // Append to targets_.
  targets_.push_back(target);

  // Allocate associated protobuf.
  PgsqlExpressionPB *expr_pb = AllocTargetPB();

  // Prepare expression. Except for constants and place_holders, all other expressions can be
  // evaluate just one time during prepare.
  RETURN_NOT_OK(target->PrepareForRead(this, expr_pb));

  // Link the given expression "attr_value" with the allocated protobuf. Note that except for
  // constants and place_holders, all other expressions can be setup just one time during prepare.
  // Example:
  // - Bind values for a target of SELECT
  //   SELECT AVG(col + ?) FROM a_table;
  expr_binds_[expr_pb] = target;
  return Status::OK();
}

Status PgDml::PrepareColumnForRead(int attr_num, PgsqlExpressionPB *target_pb,
                                   const PgColumn **col) {
  *col = nullptr;
  PgColumn *pg_col;
  RETURN_NOT_OK(FindColumn(attr_num, &pg_col));
  *col = pg_col;

  // Prepare protobuf to send to DocDB.
  target_pb->set_column_id(pg_col->id());

  // Mark non-virtual column reference for DocDB.
  if (!pg_col->is_virtual_column()) {
    pg_col->set_read_requested(true);
  }

  return Status::OK();
}

Status PgDml::PrepareColumnForWrite(PgColumn *pg_col, PgsqlExpressionPB *assign_pb) {
  // Prepare protobuf to send to DocDB.
  assign_pb->set_column_id(pg_col->id());

  // Mark non-virtual column reference for DocDB.
  if (!pg_col->is_virtual_column()) {
    pg_col->set_write_requested(true);
  }

  return Status::OK();
}

void PgDml::SetColumnRefIds(PgTableDesc::ScopedRefPtr table_desc, PgsqlColumnRefsPB *column_refs) {
  column_refs->Clear();
  for (const PgColumn& col : table_desc->columns()) {
    if (col.read_requested() || col.write_requested()) {
      column_refs->add_ids(col.id());
    }
  }
}

//--------------------------------------------------------------------------------------------------

Status PgDml::BindColumn(int attr_num, PgExpr *attr_value) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindColumn(attr_num, &col));

  // Check datatype.
  // TODO(neil) Current code combine TEXT and BINARY datatypes into ONE representation.  Once that
  // is fixed, we can remove the special if() check for BINARY type.
  if (col->internal_type() != InternalType::kBinaryValue) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *bind_pb = col->bind_pb();
  if (bind_pb == nullptr) {
    bind_pb = AllocColumnBindPB(col);
  } else {
    if (expr_binds_.find(bind_pb) != expr_binds_.end()) {
      LOG(WARNING) << strings::Substitute("Column $0 is already bound to another value.", attr_num);
    }
  }

  // Link the expression and protobuf. During execution, expr will write result to the pb.
  RETURN_NOT_OK(attr_value->PrepareForRead(this, bind_pb));

  // Link the given expression "attr_value" with the allocated protobuf. Note that except for
  // constants and place_holders, all other expressions can be setup just one time during prepare.
  // Examples:
  // - Bind values for primary columns in where clause.
  //     WHERE hash = ?
  // - Bind values for a column in INSERT statement.
  //     INSERT INTO a_table(hash, key, col) VALUES(?, ?, ?)
  expr_binds_[bind_pb] = attr_value;
  if (attr_num == static_cast<int>(PgSystemAttrNum::kYBTupleId)) {
    CHECK(attr_value->is_constant()) << "Column ybctid must be bound to constant";
    ybctid_bind_ = static_cast<PgConstant*>(attr_value)->binary_value();
  }
  return Status::OK();
}

Status PgDml::UpdateBindPBs() {
  // Process the column binds for two cases.
  // For performance reasons, we might evaluate these expressions together with bind values in YB.
  for (const auto &entry : expr_binds_) {
    PgsqlExpressionPB *expr_pb = entry.first;
    PgExpr *attr_value = entry.second;
    RETURN_NOT_OK(attr_value->Eval(this, expr_pb));
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgDml::AssignColumn(int attr_num, PgExpr *attr_value) {
  // Find column.
  PgColumn *col = nullptr;
  RETURN_NOT_OK(FindColumn(attr_num, &col));

  // Check datatype.
  // TODO(neil) Current code combine TEXT and BINARY datatypes into ONE representation.  Once that
  // is fixed, we can remove the special if() check for BINARY type.
  if (col->internal_type() != InternalType::kBinaryValue) {
    SCHECK_EQ(col->internal_type(), attr_value->internal_type(), Corruption,
              "Attribute value type does not match column type");
  }

  // Alloc the protobuf.
  PgsqlExpressionPB *assign_pb = col->assign_pb();
  if (assign_pb == nullptr) {
    assign_pb = AllocColumnAssignPB(col);
  } else {
    if (expr_assigns_.find(assign_pb) != expr_assigns_.end()) {
      return STATUS_SUBSTITUTE(InvalidArgument,
                               "Column $0 is already assigned to another value", attr_num);
    }
  }

  // Link the expression and protobuf. During execution, expr will write result to the pb.
  // - Prepare the left hand side for write.
  // - Prepare the right hand side for read. Currently, the right hand side is always constant.
  RETURN_NOT_OK(PrepareColumnForWrite(col, assign_pb));
  RETURN_NOT_OK(attr_value->PrepareForRead(this, assign_pb));

  // Link the given expression "attr_value" with the allocated protobuf. Note that except for
  // constants and place_holders, all other expressions can be setup just one time during prepare.
  // Examples:
  // - Setup rhs values for SET column = assign_pb in UPDATE statement.
  //     UPDATE a_table SET col = assign_expr;
  expr_assigns_[assign_pb] = attr_value;

  return Status::OK();
}

Status PgDml::UpdateAssignPBs() {
  // Process the column binds for two cases.
  // For performance reasons, we might evaluate these expressions together with bind values in YB.
  for (const auto &entry : expr_assigns_) {
    PgsqlExpressionPB *expr_pb = entry.first;
    PgExpr *attr_value = entry.second;
    RETURN_NOT_OK(attr_value->Eval(this, expr_pb));
  }

  return Status::OK();
}

//--------------------------------------------------------------------------------------------------

Status PgDml::Fetch(int32_t natts,
                    uint64_t *values,
                    bool *isnulls,
                    PgSysColumns *syscols,
                    bool *has_data) {
  // Each isnulls and values correspond (in order) to columns from the table schema.
  // Initialize to nulls for any columns not present in result.
  if (isnulls) {
    memset(isnulls, true, natts * sizeof(bool));
  }
  if (syscols) {
    memset(syscols, 0, sizeof(PgSysColumns));
  }

  // Load data from cache in doc_op_ to cursor_ if it is not pointing to any data.
  if (cursor_.empty()) {
    int64_t row_count = 0;
    // Keep reading untill we either reach the end or get some rows.
    while (row_count == 0) {
      if (VERIFY_RESULT(doc_op_->EndOfResult())) {
        // To be compatible with Postgres code, memset output array with 0.
        *has_data = false;
        return Status::OK();
      }

      // Read from cache.
      RETURN_NOT_OK(doc_op_->GetResult(&row_batch_));
      RETURN_NOT_OK(PgDocData::LoadCache(row_batch_, &row_count, &cursor_));
    }

    accumulated_row_count_ += row_count;
  }

  // Read the tuple from cached buffer and write it to postgres buffer.
  *has_data = true;
  PgTuple pg_tuple(values, isnulls, syscols);
  RETURN_NOT_OK(WritePgTuple(&pg_tuple));

  return Status::OK();
}

Status PgDml::WritePgTuple(PgTuple *pg_tuple) {
  for (const PgExpr *target : targets_) {
    if (target->opcode() != PgColumnRef::Opcode::PG_EXPR_COLREF) {
      return STATUS(InternalError, "Unexpected expression, only column refs supported here");
    }
    const auto *col_ref = static_cast<const PgColumnRef *>(target);
    PgWireDataHeader header = PgDocData::ReadDataHeader(&cursor_);
    target->TranslateData(&cursor_, header, col_ref->attr_num() - 1, pg_tuple);
  }
  return Status::OK();
}

}  // namespace pggate
}  // namespace yb
