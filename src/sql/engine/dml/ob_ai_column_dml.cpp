/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/dml/ob_ai_column_dml.h"
#include "sql/engine/dml/ob_dml_service.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "share/schema/ob_ai_column_schema.h"
#include "lib/string/ob_sql_string.h"
#include <vector>
#include <memory>

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share::schema;
namespace
{
int ai_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("`"));
  for (int32_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
    if (name.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(ObString(1, name.ptr() + i)));
  }
  OZ(sql.append("`"));
  return ret;
}
int ai_text_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("CONVERT(X'"));
  for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    OZ(sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i])));
  }
  OZ(sql.append("' USING utf8mb4)"));
  return ret;
}
}

int AiColumnDml::after_row(ObDMLModifyRowNode &row)
{
  int ret = OB_SUCCESS;
  if (row.dml_op_ == nullptr || row.dml_ctdef_ == nullptr) { return OB_INVALID_ARGUMENT; }
  ObTableModifyOp &op = *row.dml_op_;
  const ObDMLBaseCtDef &ctdef = *row.dml_ctdef_;
  ObSQLSessionInfo *session = op.get_exec_ctx().get_my_session();
  if (!ctdef.has_ai_columns_ || session == nullptr || session->is_ai_column_internal_write()) { return ret; }
  ObArenaAllocator allocator("AiColumnT1");
  ObSchemaGetterGuard *schema = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  const ObTableSchema *table = nullptr;
  const ObDatabaseSchema *database = nullptr;
  if (schema == nullptr) { return OB_NOT_INIT; }
  OZ(schema->get_table_schema(ctdef.das_base_ctdef_.index_tid_, table));
  if (OB_SUCC(ret) && table == nullptr) { ret = OB_TABLE_NOT_EXIST; }
  if (OB_SUCC(ret)) { ret = schema->get_database_schema(table->get_database_id(), database); }
  if (OB_SUCC(ret) && database == nullptr) { ret = OB_ERR_BAD_DATABASE; }
  const bool inserting = row.dml_event_ == ObDmlEventType::DE_INSERTING;
  const bool deleting = row.dml_event_ == ObDmlEventType::DE_DELETING;
  auto position = [&](uint64_t id) -> int64_t {
    for (int64_t i = 0; i < ctdef.column_ids_.count(); ++i) {
      if (ctdef.column_ids_.at(i) == id) { return i; }
    }
    return -1;
  };
  auto changed = [&](uint64_t id, bool &different) -> int {
    int ret = OB_SUCCESS;
    different = inserting;
    const int64_t index = position(id);
    if (index < 0 || index >= ctdef.old_row_.count() || index >= ctdef.new_row_.count()) { return OB_ERR_UNEXPECTED; }
    ObExpr *old_expr = ctdef.old_row_.at(index);
    ObExpr *new_expr = ctdef.new_row_.at(index);
    ObDatum *old_value = nullptr;
    ObDatum *new_value = nullptr;
    if (old_expr == nullptr || new_expr == nullptr) { return OB_ERR_UNEXPECTED; }
    OZ(old_expr->eval(op.get_eval_ctx(), old_value));
    OZ(new_expr->eval(op.get_eval_ctx(), new_value));
    if (OB_SUCC(ret)) {
      if (!old_value->is_null() && !new_value->is_null() &&
          is_lob_storage(old_expr->datum_meta_.type_) && is_lob_storage(new_expr->datum_meta_.type_)) {
        int64_t comparison = 0;
        OZ(ObDMLService::check_lob_column_changed(op.get_eval_ctx(), *old_expr, *old_value, *new_expr, *new_value, comparison));
        different = comparison != 0;
      } else { different = !ObDatum::binary_equal(*old_value, *new_value); }
    }
    return ret;
  };
  auto predicate = [&](const ObExprPtrIArray &values, ObSqlString &sql) -> int {
    int ret = OB_SUCCESS;
    const ObRowkeyInfo &keys = table->get_rowkey_info();
    if (keys.get_size() == 0) { return OB_NOT_SUPPORTED; }
    for (int64_t i = 0; OB_SUCC(ret) && i < keys.get_size(); ++i) {
      const uint64_t id = keys.get_column(i)->column_id_;
      const int64_t index = position(id);
      const ObColumnSchemaV2 *column = table->get_column_schema(id);
      if (column == nullptr || index < 0 || index >= values.count()) { ret = OB_ERR_UNEXPECTED; }
      else {
        if (i != 0) { OZ(sql.append(" AND ")); }
        OZ(ai_identifier(sql, column->get_column_name_str()));
        OZ(sql.append(" = "));
        ObDatum *datum = nullptr;
        ObObj value;
        OZ(values.at(index)->eval(op.get_eval_ctx(), datum));
        if (OB_SUCC(ret) && datum->is_null()) { ret = OB_ERR_NULL_VALUE; }
        if (OB_SUCC(ret)) { ret = datum->to_obj(value, values.at(index)->obj_meta_); }
        // The persisted predicate also identifies task rows. Its bytes and
        // interpretation must not depend on the writer's SQL mode or time zone.
        if (OB_SUCC(ret) && value.is_string_type()) {
          const bool binary = column->get_collation_type() == CS_TYPE_BINARY;
          OZ(sql.append(binary ? "X'" : "CONVERT(X'"));
          const ObString bytes = value.get_string();
          for (int32_t j = 0; OB_SUCC(ret) && j < bytes.length(); ++j) {
            OZ(sql.append_fmt("%02x", static_cast<unsigned char>(bytes.ptr()[j])));
          }
          if (binary) { OZ(sql.append("'")); }
          else {
            OZ(sql.append_fmt("' USING %s)", ObCharset::charset_name(
                ObCharset::charset_type_by_coll(column->get_collation_type()))));
          }
        } else if (OB_SUCC(ret) && value.is_timestamp() && value.get_timestamp() >= 0) {
          OZ(sql.append_fmt("FROM_UNIXTIME(%ld.%06ld)", value.get_timestamp() / 1000000, value.get_timestamp() % 1000000));
        } else {
          char *buffer = nullptr;
          int64_t length = 0;
          int64_t pos = 0;
          OZ(value.print_sql_literal(buffer, length, pos, allocator, ObObjPrintParams()));
          OZ(sql.append(ObString(pos, buffer)));
        }
      }
    }
    return ret;
  };
  bool key_changed = false;
  for (int64_t i = 0; OB_SUCC(ret) && !inserting && !deleting && !key_changed &&
      i < table->get_rowkey_info().get_size(); ++i) {
    ret = changed(table->get_rowkey_info().get_column(i)->column_id_, key_changed);
  }
  std::vector<const ObColumnSchemaV2 *> columns;
  std::vector<AiColumnDefinition *> definitions;
  using DefinitionPtr = std::unique_ptr<AiColumnDefinition, void (*)(AiColumnDefinition *)>;
  std::vector<DefinitionPtr> owned;
  for (int64_t i = 0; OB_SUCC(ret) && !deleting && i < table->get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
    if (column != nullptr && column->is_ai_column()) {
      AiColumnDefinition *definition = OB_NEWx(AiColumnDefinition, &allocator);
      if (definition == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else {
        owned.emplace_back(definition, [](AiColumnDefinition *value) { value->~AiColumnDefinition(); });
        OZ(definition->parse(allocator, column->get_ai_column_definition()));
        bool dirty = inserting || key_changed;
        for (int64_t j = 0; OB_SUCC(ret) && !dirty && j < definition->dependencies_.count(); ++j) {
          ret = changed(definition->dependencies_.at(j), dirty);
        }
        if (OB_SUCC(ret) && dirty) { columns.push_back(column); definitions.push_back(definition); }
      }
    }
  }
  if (OB_SUCC(ret) && !deleting && definitions.empty()) { return ret; }
  ObSqlString old_key;
  ObSqlString new_key;
  ObSqlString target;
  if (OB_SUCC(ret) && !inserting) { ret = predicate(ctdef.old_row_, old_key); }
  if (OB_SUCC(ret) && !deleting) { ret = predicate(ctdef.new_row_, new_key); }
  if (OB_SUCC(ret)) { ret = ai_identifier(target, database->get_database_name_str()); }
  OZ(target.append("."));
  if (OB_SUCC(ret)) { ret = ai_identifier(target, table->get_table_name_str()); }
  char request[UuidCommon::LENGTH_UUID];
  if (OB_SUCC(ret) && !deleting) { ret = ObExprUuid::gen_server_uuid(request, sizeof(request)); }
  const ObString request_id(sizeof(request), request);
  // Use the external session's inner connection: these writes belong to the
  // business transaction and its statement savepoint, including rollback.
  OZ(op.open_inner_conn());
  bool nested = false;
  const bool saved_internal = session->is_ai_column_internal_write();
  const bool saved_uncommitted = op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_;
  if (OB_SUCC(ret)) {
    session->set_ai_column_internal_write(true);
    op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    OZ(op.begin_nested_session(true));
    nested = OB_SUCC(ret);
  }
  if (OB_SUCC(ret) && (deleting || key_changed)) {
    ObSqlString sql;
    OZ(sql.append_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id=%lu AND rowkey_hash=SHA2(", table->get_table_id()));
    OZ(ai_text_literal(sql, old_key.string()));
    OZ(sql.append(",256)"));
    OZ(op.execute_write(sql.ptr()));
  }
  if (OB_SUCC(ret) && !deleting) {
    ObSqlString update;
    OZ(update.append_fmt("UPDATE %s SET ", target.ptr()));
    for (size_t i = 0; OB_SUCC(ret) && i < definitions.size(); ++i) {
      const AiColumnDefinition &definition = *definitions[i];
      const uint64_t ids[] = {columns[i]->get_column_id(), definition.state_column_id_,
          definition.request_column_id_, definition.result_spec_column_id_};
      for (int j = 0; OB_SUCC(ret) && j < 4; ++j) {
        const ObColumnSchemaV2 *column = table->get_column_schema(ids[j]);
        if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
        else {
          if (i != 0 || j != 0) { OZ(update.append(", ")); }
          OZ(ai_identifier(update, column->get_column_name_str()));
          OZ(update.append("="));
          if (j == 0) { OZ(update.append("NULL")); }
          else if (j == 1) { OZ(update.append("0")); }
          else if (j == 2) { OZ(ai_text_literal(update, request_id)); }
          else { OZ(update.append_fmt("'%lu'", definition.definition_version_)); }
        }
      }
    }
    OZ(update.append_fmt(" WHERE %s", new_key.ptr()));
    OZ(op.execute_write(update.ptr()));
    for (size_t i = 0; OB_SUCC(ret) && i < definitions.size(); ++i) {
      const AiColumnDefinition &definition = *definitions[i];
      ObSqlString task;
      OZ(task.append_fmt("REPLACE INTO oceanbase.__all_ai_column_task(table_id,rowkey_hash,column_id,request_id,definition_version,rowkey_predicate,state,retry_count,eligible_ts) VALUES(%lu,SHA2(", table->get_table_id()));
      OZ(ai_text_literal(task, new_key.string()));
      OZ(task.append_fmt(",256),%lu,", columns[i]->get_column_id()));
      OZ(ai_text_literal(task, request_id));
      OZ(task.append_fmt(",%lu,", definition.definition_version_));
      OZ(ai_text_literal(task, new_key.string()));
      OZ(task.append(",0,0,NOW(6))"));
      OZ(op.execute_write(task.ptr()));
    }
  }
  if (nested) {
    const int end = op.end_nested_session();
    if (OB_SUCC(ret)) { ret = end; }
  }
  session->set_ai_column_internal_write(saved_internal);
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = saved_uncommitted;
  return ret;
}
}
}
