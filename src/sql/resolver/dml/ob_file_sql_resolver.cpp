/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/dml/ob_dml_resolver.h"
#include "sql/resolver/dml/ob_select_resolver.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/ob_sql_utils.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"

namespace oceanbase
{
namespace sql
{
using namespace common;

namespace
{
int file_sql_syntax_error()
{
  LOG_USER_ERROR(OB_ERR_PARSE_SQL, "Invalid File SQL syntax", 0, "", 1);
  return OB_ERR_PARSE_SQL;
}
}

int ObDMLResolver::resolve_file_sql_columns(const TableItem &table, ObIArray<ColumnItem> &columns)
{
  int ret = OB_SUCCESS;
  ObDMLStmt *stmt = get_stmt();
  if (stmt == nullptr) { ret = OB_NOT_INIT; }
  else {
    for (int64_t i = 0; OB_SUCC(ret) && i < stmt->get_column_size(); ++i) {
      const ColumnItem *column = stmt->get_column_item(i);
      if (column != nullptr && column->table_id_ == table.table_id_) { ret = columns.push_back(*column); }
    }
  }
  return ret;
}

int ObDMLResolver::resolve_file_sql_table_item(const ParseNode &node, TableItem *&table_item)
{
  int ret = OB_SUCCESS;
  table_item = nullptr;
  ObDMLStmt *stmt = get_stmt();
  if (node.num_child_ != 3 || node.children_[0] == nullptr || node.children_[1] == nullptr ||
      session_info_ == nullptr || allocator_ == nullptr || stmt == nullptr || params_.expr_factory_ == nullptr) {
    return OB_INVALID_ARGUMENT;
  }
  const ParseNode &function = *node.children_[0];
  const ParseNode &arguments = *node.children_[1];
  ObString function_name(static_cast<int32_t>(function.str_len_), function.str_value_);
  ObString format;
  if (function_name.case_compare("read_csv") == 0) { format = ObString("csv"); }
  else if (function_name.case_compare("read_json") == 0 || function_name.case_compare("read_ndjson") == 0) { format = ObString("json"); }
  else if (function_name.case_compare("read_parquet") == 0) { format = ObString("parquet"); }
  else if (function_name.case_compare("glob") == 0) { format = ObString("glob"); }
  else { return file_sql_syntax_error(); }
  if (arguments.num_child_ < 1 || arguments.children_[0] == nullptr) { return file_sql_syntax_error(); }
  if (OB_FAIL(FileSqlReader::check_enabled(*session_info_))) { return ret; }

  auto constant = [&](const ParseNode &value_node, ObObj &value) -> int {
    int ret = OB_SUCCESS;
    ObRawExpr *expr = nullptr;
    if (OB_FAIL(resolve_sql_expr(value_node, expr))) {
    } else if (expr == nullptr || expr->has_flag(CNT_COLUMN) || expr->has_flag(CNT_SUB_QUERY)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(expr->deduce_type(session_info_))) {
    } else if (OB_FAIL(ObSQLUtils::calc_simple_expr_without_row(session_info_, expr, value,
                      params_.param_list_, *allocator_))) {
    }
    return ret;
  };
  ObObj path_value;
  ObString path;
  if (OB_FAIL(constant(*arguments.children_[0], path_value))) {
  } else if (path_value.is_null() || !ob_is_string_type(path_value.get_type())) {
    ret = OB_INVALID_ARGUMENT;
  } else { path = path_value.get_string(); }

  ObJsonObject options(allocator_);
  for (int64_t i = 1; OB_SUCC(ret) && i < arguments.num_child_; ++i) {
    const ParseNode *argument = arguments.children_[i];
    if (argument == nullptr || argument->type_ != T_OP_EQ || argument->num_child_ != 2 ||
        argument->children_[0] == nullptr || argument->children_[1] == nullptr ||
        argument->children_[0]->type_ != T_COLUMN_REF) {
      ret = file_sql_syntax_error();
    } else {
      const ParseNode *key_node = argument->children_[0];
      if (key_node->num_child_ != 3 || key_node->children_[2] == nullptr ||
          key_node->children_[0] != nullptr || key_node->children_[1] != nullptr) {
        ret = file_sql_syntax_error();
      } else {
        key_node = key_node->children_[2];
        ObString key(static_cast<int32_t>(key_node->str_len_), key_node->str_value_);
        ObString normalized;
        ObObj value;
        ObJsonNode *json = nullptr;
        if (OB_FAIL(ob_write_string(*allocator_, key, normalized))) {
        } else {
          for (int32_t j = 0; j < normalized.length(); ++j) {
            if (normalized.ptr()[j] >= 'A' && normalized.ptr()[j] <= 'Z') { normalized.ptr()[j] += 'a' - 'A'; }
          }
          if (options.get_value(normalized) != nullptr) { ret = OB_INVALID_ARGUMENT; }
          else if (OB_FAIL(constant(*argument->children_[1], value))) {
          } else if (value.is_null()) { ret = OB_INVALID_ARGUMENT; }
          else if (normalized == "header") {
            if (!ob_is_integer_type(value.get_type()) || (value.get_int() != 0 && value.get_int() != 1)) {
              ret = OB_INVALID_ARGUMENT;
            } else { json = OB_NEWx(ObJsonBoolean, allocator_, value.get_int() != 0); }
          } else if (normalized == "sample_size") {
            if (!ob_is_integer_type(value.get_type())) { ret = OB_INVALID_ARGUMENT; }
            else { json = OB_NEWx(ObJsonInt, allocator_, value.get_int()); }
          } else if (ob_is_string_type(value.get_type())) {
            json = OB_NEWx(ObJsonString, allocator_, value.get_string());
          } else { ret = OB_INVALID_ARGUMENT; }
          if (OB_SUCC(ret)) {
            if (json == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
            else { ret = options.add(normalized, json); }
          }
        }
      }
    }
  }
  ObJsonBuffer options_buffer(allocator_);
  ObString options_text;
  ObString definition;
  ObSEArray<FileSqlColumn, 16> columns;
  FileSqlReader reader;
  if (OB_FAIL(ret)) {
  } else if (format == "glob" && arguments.num_child_ != 1) {
    ret = file_sql_syntax_error();
  } else if (OB_FAIL(options.print(options_buffer, false))) {
  } else if (OB_FAIL(ob_write_string(*allocator_, options_buffer.string(), options_text))) {
  } else if (OB_FAIL(reader.infer(*session_info_, session_info_->get_cur_exec_ctx(), *allocator_,
                 path, format, options_text, definition, columns))) {
  } else {
    ObConstRawExpr *value_expr = nullptr;
    if (OB_FAIL(params_.expr_factory_->create_raw_expr(T_VARCHAR, value_expr))) {
    } else {
      ObObj value;
      value.set_varchar(definition);
      value.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      value_expr->set_value(value);
      value_expr->set_collation_level(CS_LEVEL_COERCIBLE);
      value_expr->set_accuracy(ObAccuracy::DDL_DEFAULT_ACCURACY[ObVarcharType]);
      if ((table_item = stmt->create_table_item(*allocator_)) == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
      else {
        ObString alias = function_name;
        if (node.children_[2] != nullptr) {
          alias.assign_ptr(node.children_[2]->str_value_, static_cast<int32_t>(node.children_[2]->str_len_));
        }
        table_item->table_id_ = generate_table_id();
        table_item->table_name_ = alias;
        table_item->alias_name_ = alias;
        table_item->type_ = TableItem::FUNCTION_TABLE;
        table_item->is_file_sql_ = true;
        table_item->is_system_table_ = false;
        table_item->function_table_expr_ = value_expr;
        if (OB_FAIL(stmt->add_table_item(session_info_, table_item))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
          ColumnItem *column = nullptr;
          const FileSqlColumn &meta = columns.at(i);
          if (OB_FAIL(resolve_function_table_column_item(*table_item, meta.meta_, meta.accuracy_,
                       meta.name_, OB_APP_MIN_COLUMN_ID + i, column))) {
          } else if (!meta.nullable_) { column->expr_->set_result_flag(NOT_NULL_FLAG); }
        }
        if (OB_SUCC(ret) && params_.query_ctx_ != nullptr) { params_.query_ctx_->has_file_sql_ = true; }
      }
    }
  }
  return ret;
}
}
}

namespace oceanbase
{
namespace sql
{
int ObSelectResolver::resolve_file_sql_copy(const ParseNode &node, ObSelectIntoItem &into_item)
{
  int ret = OB_SUCCESS;
  if (node.num_child_ != 2 || node.children_[0] == nullptr || session_info_ == nullptr ||
      allocator_ == nullptr || get_select_stmt() == nullptr) { return OB_INVALID_ARGUMENT; }
  if (OB_FAIL(FileSqlReader::check_enabled(*session_info_))) { return ret; }
  auto constant = [&](const ParseNode &node, ObObj &value) -> int {
    int ret = OB_SUCCESS;
    ObRawExpr *expr = nullptr;
    if (OB_FAIL(resolve_sql_expr(node, expr))) {
    } else if (expr == nullptr || expr->has_flag(CNT_COLUMN) || expr->has_flag(CNT_SUB_QUERY)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(expr->deduce_type(session_info_))) {
    } else { ret = ObSQLUtils::calc_simple_expr_without_row(session_info_, expr, value,
                      params_.param_list_, *allocator_); }
    return ret;
  };
  if (OB_FAIL(constant(*node.children_[0], into_item.outfile_name_))) { return ret; }
  if (into_item.outfile_name_.is_null() || !ob_is_string_type(into_item.outfile_name_.get_type())) {
    return OB_INVALID_ARGUMENT;
  }
  ObJsonObject options(allocator_);
  const ParseNode *list = node.children_[1];
  for (int64_t i = 0; OB_SUCC(ret) && list != nullptr && i < list->num_child_; ++i) {
    const ParseNode *entry = list->children_[i];
    if (entry == nullptr || entry->num_child_ != 2 || entry->children_[0] == nullptr ||
        entry->children_[1] == nullptr) { ret = file_sql_syntax_error(); }
    else {
      const ParseNode &name = *entry->children_[0];
      const ParseNode &value = *entry->children_[1];
      ObString key(static_cast<int32_t>(name.str_len_), name.str_value_);
      ObString normalized;
      ObObj option;
      ObJsonNode *json = nullptr;
      if (OB_FAIL(ob_write_string(*allocator_, key, normalized))) {
      } else {
        for (int32_t j = 0; j < normalized.length(); ++j) {
          if (normalized.ptr()[j] >= 'A' && normalized.ptr()[j] <= 'Z') { normalized.ptr()[j] += 'a' - 'A'; }
        }
        if (options.get_value(normalized) != nullptr) { ret = OB_INVALID_ARGUMENT; }
        else if (value.type_ == T_IDENT) {
          option.set_varchar(ObString(static_cast<int32_t>(value.str_len_), value.str_value_));
        } else { ret = constant(value, option); }
        if (OB_SUCC(ret)) {
          if (normalized == "header") {
            if (!ob_is_integer_type(option.get_type()) || (option.get_int() != 0 && option.get_int() != 1)) {
              ret = OB_INVALID_ARGUMENT;
            } else { json = OB_NEWx(ObJsonBoolean, allocator_, option.get_int() != 0); }
          } else if (normalized == "format" || normalized == "delimiter" || normalized == "quote" ||
                     normalized == "escape" || normalized == "nullstr") {
            if (!ob_is_string_type(option.get_type())) { ret = OB_INVALID_ARGUMENT; }
            else { json = OB_NEWx(ObJsonString, allocator_, option.get_string()); }
          } else { ret = OB_INVALID_ARGUMENT; }
          if (OB_SUCC(ret)) {
            if (json == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
            else { ret = options.add(normalized, json); }
          }
        }
      }
    }
  }
  ObJsonBuffer buffer(allocator_);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(options.print(buffer, false))) {
  } else if (OB_FAIL(ob_write_string(*allocator_, buffer.string(), into_item.external_properties_))) {
  } else {
    into_item.is_single_ = true;
    if (params_.query_ctx_ != nullptr) { params_.query_ctx_->has_file_sql_ = true; }
  }
  return ret;
}

int ObSelectResolver::finalize_file_sql_copy(ObSelectIntoItem &into_item)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  ObJsonArray headers(allocator_);
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(allocator_, into_item.external_properties_,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base))) {
  } else if (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT) { ret = OB_ERR_UNEXPECTED; }
  for (int64_t i = 0; OB_SUCC(ret) && i < get_select_stmt()->get_select_item_size(); ++i) {
    const SelectItem &item = get_select_stmt()->get_select_item(i);
    ObJsonString *name = OB_NEWx(ObJsonString, allocator_, item.alias_name_.empty() ? item.expr_name_ : item.alias_name_);
    if (name == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = headers.append(name); }
  }
  ObJsonObject definition(allocator_);
  ObJsonBuffer buffer(allocator_);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(definition.add("options", static_cast<ObJsonObject *>(base)))) {
  } else if (OB_FAIL(definition.add("headers", &headers))) {
  } else if (OB_FAIL(definition.print(buffer, false))) {
  } else { ret = ob_write_string(*allocator_, buffer.string(), into_item.external_properties_); }
  return ret;
}
}
}
