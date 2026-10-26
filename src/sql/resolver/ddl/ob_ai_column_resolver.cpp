/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/ddl/ob_ddl_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "share/schema/ob_ai_column_schema.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"
#include "sql/printer/ob_raw_expr_printer.h"
#include "sql/ob_sql_utils.h"
#include "query/vector/ob_vector_index_util.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include <set>

namespace oceanbase
{
namespace sql
{
using namespace common;
using namespace share;
using namespace share::schema;

int ObAlterTableResolver::resolve_ai_column_retry(ObAlterTableStmt &statement, const ParseNode &action)
{
  int ret = OB_SUCCESS;
  if (table_schema_ == nullptr || action.num_child_ != 2 || action.children_[0] == nullptr ||
      !table_schema_->is_user_table() || table_schema_->is_view_table()) { return OB_NOT_SUPPORTED; }
  const ParseNode &name = *action.children_[0];
  const ObColumnSchemaV2 *column = table_schema_->get_column_schema(ObString(name.str_len_, name.str_value_));
  if (column == nullptr || !column->is_ai_column()) {
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
    return OB_NOT_SUPPORTED;
  }
  AiColumnDefinition definition;
  OZ(definition.parse(*allocator_, column->get_ai_column_definition()));
  ObSqlString rewritten;
  if (OB_SUCC(ret) && action.children_[1] != nullptr) {
    const ObString original(action.str_len_, action.str_value_);
    ObSEArray<const ParseNode *, 16> nodes;
    std::set<int64_t> positions;
    OZ(nodes.push_back(action.children_[1]));
    for (int64_t i = 0; OB_SUCC(ret) && i < nodes.count(); ++i) {
      const ParseNode *node = nodes.at(i);
      if (node == nullptr || node->type_ == T_SELECT) { continue; }
      if (node->type_ == T_COLUMN_REF && node->num_child_ == 3 &&
          node->children_[0] == nullptr && node->children_[1] == nullptr && node->children_[2] != nullptr) {
        const ParseNode &identifier = *node->children_[2];
        if (!identifier.is_input_quoted_ && ObString(identifier.str_len_, identifier.str_value_).case_compare("status") == 0) {
          const int64_t offset = identifier.sql_str_off_ - action.value_;
          if (offset < 0 || offset + 6 > original.length() ||
              ObString(6, original.ptr() + offset).case_compare("status") != 0) { ret = OB_ERR_UNEXPECTED; }
          else { positions.insert(offset); }
        }
      }
      for (int64_t j = 0; OB_SUCC(ret) && j < node->num_child_; ++j) { OZ(nodes.push_back(node->children_[j])); }
    }
    const ObColumnSchemaV2 *state = table_schema_->get_column_schema(definition.state_column_id_);
    const ObColumnSchemaV2 *spec = table_schema_->get_column_schema(definition.result_spec_column_id_);
    if (OB_SUCC(ret) && (state == nullptr || spec == nullptr)) { ret = OB_ERR_UNEXPECTED; }
    int64_t copied = 0;
    // Hidden names are generated numeric identifiers, never caller-controlled SQL.
    for (const int64_t offset : positions) {
      OZ(rewritten.append(ObString(offset - copied, original.ptr() + copied)));
      if (OB_SUCC(ret)) {
        OZ(rewritten.append_fmt("(CASE WHEN `%.*s`=1 AND `%.*s`='%lu' THEN 'READY' "
            "WHEN `%.*s`=2 AND `%.*s`='%lu' THEN 'FAILED' ELSE 'PENDING' END)",
            state->get_column_name_str().length(), state->get_column_name_str().ptr(),
            spec->get_column_name_str().length(), spec->get_column_name_str().ptr(), definition.definition_version_,
            state->get_column_name_str().length(), state->get_column_name_str().ptr(),
            spec->get_column_name_str().length(), spec->get_column_name_str().ptr(), definition.definition_version_));
      }
      copied = offset + 6;
    }
    OZ(rewritten.append(ObString(original.length() - copied, original.ptr() + copied)));
  }
  ObString predicate;
  OZ(ob_write_string(*allocator_, rewritten.string(), predicate));
  if (OB_SUCC(ret)) { statement.set_ai_column_retry(column->get_column_id(), predicate); }
  return ret;
}

int ObDDLResolver::resolve_ai_column_definition(ObColumnSchemaV2 &column, const ParseNode &node)
{
  int ret = OB_SUCCESS;
  if (session_info_ == nullptr || allocator_ == nullptr || node.num_child_ != 6 ||
      node.children_[4] == nullptr) { return OB_INVALID_ARGUMENT; }
  AiColumnDefinition definition;
  definition.expression_.assign_ptr(node.children_[4]->str_value_, node.children_[4]->str_len_);
  const ParseNode *options = node.children_[5];
  bool saw_retries = false;
  bool saw_mode = false;
  for (int64_t i = 0; OB_SUCC(ret) && options != nullptr && i < options->num_child_; ++i) {
    const ParseNode *option = options->children_[i];
    if (option == nullptr || option->type_ != T_OP_EQ || option->num_child_ != 2 ||
        option->children_[0] == nullptr || option->children_[1] == nullptr) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const ParseNode *key = option->children_[0];
      const ParseNode *value = option->children_[1];
      if (key->type_ != T_COLUMN_REF || key->num_child_ != 3 ||
          key->children_[0] != nullptr || key->children_[1] != nullptr || key->children_[2] == nullptr) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        key = key->children_[2];
        ObString name(key->str_len_, key->str_value_);
        if (name.case_compare("sync_mode") == 0 && !saw_mode) {
          saw_mode = true;
          if (value->type_ != T_VARCHAR || ObString(value->str_len_, value->str_value_).case_compare("async") != 0) {
            ret = OB_NOT_SUPPORTED;
          }
        } else if (name.case_compare("max_retries") == 0 && !saw_retries) {
          saw_retries = true;
          if (value->type_ != T_INT || value->value_ < 0 || value->value_ > 100) { ret = OB_INVALID_ARGUMENT; }
          else { definition.max_retries_ = value->value_; }
        } else { ret = OB_INVALID_ARGUMENT; }
      }
    }
  }
  ObString stored;
  OZ(definition.serialize(*allocator_, stored));
  OZ(column.set_ai_column_definition(stored));
  return ret;
}

int ObDDLResolver::finalize_ai_column(ObColumnSchemaV2 &column, ObTableSchema &table,
    uint64_t &max_column_id, ObIArray<ObColumnSchemaV2> &hidden, const ObColumnSchemaV2 *original)
{
  int ret = OB_SUCCESS;
  AiColumnDefinition definition;
  ObRawExpr *expr = nullptr;
  ObSEArray<ObQualifiedName, 4> names;
  if (!column.is_ai_column()) { return ret; }
  if (!session_info_->is_inner() && params_.disable_privilege_check_ != PRIV_CHECK_FLAG_DISABLE) {
    ObSchemaGetterGuard &guard = *schema_checker_->get_schema_guard();
    ObSessionPrivInfo privileges;
    ObAIServiceEndpointPrivUtil check(guard);
    bool allowed = false;
    const uint64_t user = session_info_->get_priv_user_id() == OB_INVALID_ID ?
        OB_SYS_USER_ID : session_info_->get_priv_user_id();
    OZ(guard.get_session_priv_info(user, session_info_->get_database_name(), privileges));
    OZ(check.check_access_ai_model_priv(*allocator_, privileges, allowed));
    if (OB_SUCC(ret) && !allowed) {
      ret = OB_ERR_NO_PRIVILEGE;
      LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
    }
  }
  OZ(definition.parse(*allocator_, column.get_ai_column_definition()));
  if (OB_SUCC(ret) && definition.state_column_id_ != 0) { return ret; }
  OZ(ObRawExprUtils::build_generated_column_expr(definition.expression_, *params_.expr_factory_,
      *session_info_, expr, names, &table, nullptr, schema_checker_));
  for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
    const ObQualifiedName &name = names.at(i);
    const ObColumnSchemaV2 *dependency = table.get_column_schema(name.col_name_);
    if (name.is_sys_func() || !name.database_name_.empty() || !name.tbl_name_.empty() ||
        dependency == nullptr || dependency->is_hidden()) { ret = OB_ERR_BAD_FIELD_ERROR; }
    else if (dependency->get_column_id() == column.get_column_id() ||
             dependency->is_ai_column() || dependency->is_generated_column()) { ret = OB_NOT_SUPPORTED; }
    else if (OB_FAIL(ObRawExprUtils::init_column_expr(*dependency, session_info_, *name.ref_expr_))) {
    } else if (!has_exist_in_array(definition.dependencies_, dependency->get_column_id())) {
      ret = definition.dependencies_.push_back(dependency->get_column_id());
    }
  }
  if (OB_SUCC(ret) && expr == nullptr) { ret = OB_ERR_UNEXPECTED; }
  const bool embedding = OB_SUCC(ret) && expr->get_expr_type() == T_FUN_SYS_AI_EMBED;
  if (OB_SUCC(ret) && !embedding && expr->get_expr_type() != T_FUN_SYS_AI_COMPLETE) { ret = OB_NOT_SUPPORTED; }
  // AI calls are allowed only at the root; input and config remain ordinary SQL expressions.
  for (int64_t i = 0; OB_SUCC(ret) && i < expr->get_param_count(); ++i) {
    ObSEArray<ObRawExpr *, 8> pending;
    OZ(pending.push_back(expr->get_param_expr(i)));
    for (int64_t j = 0; OB_SUCC(ret) && j < pending.count(); ++j) {
      ObRawExpr *part = pending.at(j);
      if (part == nullptr) { ret = OB_ERR_UNEXPECTED; }
      else if (part->get_expr_type() == T_FUN_SYS_AI_COMPLETE || part->get_expr_type() == T_FUN_SYS_AI_EMBED ||
               part->get_expr_type() == T_FUN_SYS_AI_RERANK || part->is_query_ref_expr() ||
               part->is_udf_expr() || part->is_aggr_expr()) { ret = OB_NOT_SUPPORTED; }
      else {
        if (part->get_expr_type() != T_FUN_SYS_AI_PROMPT &&
            OB_FAIL(ObRawExprUtils::check_deterministic_single(part, ObResolverUtils::CHECK_FOR_GENERATED_COLUMN))) {
          ret = OB_NOT_SUPPORTED;
        }
        for (int64_t k = 0; OB_SUCC(ret) && k < part->get_param_count(); ++k) {
          ret = pending.push_back(part->get_param_expr(k));
        }
      }
    }
  }
  OZ(expr->formalize(session_info_));
  if (OB_SUCC(ret) && expr->get_param_count() >= 2) {
    definition.input_is_json_ = expr->get_param_expr(1)->get_result_type().get_type() == ObJsonType;
  }
  if (OB_SUCC(ret) && (expr->get_param_count() < 2 || expr->get_param_count() > 3 ||
      !expr->get_param_expr(0)->is_static_scalar_const_expr())) { ret = OB_INVALID_ARGUMENT; }
  ObObj model_value;
  if (OB_SUCC(ret)) {
    OZ(ObSQLUtils::calc_simple_expr_without_row(session_info_, expr->get_param_expr(0),
        model_value, params_.param_list_, *allocator_));
    if (OB_SUCC(ret) && (model_value.is_null() || !ob_is_string_type(model_value.get_type()) ||
        model_value.get_string().empty())) { ret = OB_INVALID_ARGUMENT; }
    if (OB_SUCC(ret)) { definition.model_ = model_value.get_string(); }
  }
  const ObAiModelSchema *model = nullptr;
  if (OB_SUCC(ret)) {
    OZ(schema_checker_->get_schema_guard()->get_ai_model_schema(definition.model_, model));
    if (OB_SUCC(ret) && (model == nullptr || model->get_type() !=
        (embedding ? EndpointType::DENSE_EMBEDDING : EndpointType::COMPLETION))) { ret = OB_INVALID_ARGUMENT; }
  }
  if (OB_SUCC(ret) && embedding) {
    if (!column.is_collection()) { ret = OB_NOT_SUPPORTED; }
    else { ret = ObVectorIndexUtil::get_vector_dim_from_extend_type_info(column.get_extended_type_info(), definition.dimension_); }
    if (OB_SUCC(ret) && expr->get_param_count() == 3) {
      ObObj dimension;
      ObRawExpr *dim_expr = expr->get_param_expr(2);
      if (!dim_expr->is_static_scalar_const_expr()) { ret = OB_INVALID_ARGUMENT; }
      else if (OB_FAIL(ObSQLUtils::calc_simple_expr_without_row(session_info_, dim_expr,
          dimension, params_.param_list_, *allocator_))) {
      } else if (!dimension.is_int() || dimension.get_int() <= 0) { ret = OB_INVALID_ARGUMENT; }
      else if (dimension.get_int() != definition.dimension_) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<unsigned>(definition.dimension_),
            static_cast<unsigned>(dimension.get_int()));
      }
    }
  } else if (OB_SUCC(ret)) {
    if (column.is_json()) {
      definition.system_prompt_ = ObString("Return exactly one valid JSON value. Do not include markdown or commentary.");
    } else if (ob_is_integer_type(column.get_data_type())) {
      definition.system_prompt_ = ObString("Return exactly one base-10 integer within the destination SQL integer range. Do not include quotes or commentary.");
    } else if (ob_is_numeric_type(column.get_data_type()) || ob_is_decimal_int(column.get_data_type())) {
      definition.system_prompt_ = ObString("Return exactly one finite base-10 number within the destination SQL numeric precision and scale. Do not include commentary.");
    } else if (ob_is_string_type(column.get_data_type()) || ob_is_text_tc(column.get_data_type())) {
      definition.system_prompt_ = ObString("Return only the requested plain text, without quotes, markdown or commentary.");
    } else if (ob_is_datetime_or_mysql_datetime_tc(column.get_data_type()) ||
               ob_is_date_or_mysql_date(column.get_data_type())) {
      definition.system_prompt_ = ObString("Return only one SQL date or datetime in YYYY-MM-DD HH:MM:SS format, without quotes or commentary.");
    } else { ret = OB_NOT_SUPPORTED; }
  }
  auto print = [&](ObRawExpr *part, ObString &value) -> int {
    int ret = OB_SUCCESS;
    char *buffer = static_cast<char *>(allocator_->alloc(65536));
    int64_t pos = 0;
    if (buffer == nullptr) { return OB_ALLOCATE_MEMORY_FAILED; }
    OZ(ObRawExprUtils::erase_operand_implicit_cast(part, part));
    ObRawExprPrinter printer(buffer, 65536, &pos, schema_checker_->get_schema_guard(), session_info_->get_timezone_info());
    OZ(printer.do_print(part, T_NONE_SCOPE, true));
    if (OB_SUCC(ret)) { value.assign_ptr(buffer, pos); }
    return ret;
  };
  ObString model_sql;
  OZ(print(expr->get_param_expr(0), model_sql));
  OZ(print(expr->get_param_expr(1), definition.input_expr_));
  if (OB_SUCC(ret) && !embedding && expr->get_param_count() == 3) {
    ret = print(expr->get_param_expr(2), definition.config_expr_);
  }
  ObSqlString normalized;
  if (OB_SUCC(ret)) {
    if (embedding) {
      OZ(normalized.append_fmt("AI_EMBED(%.*s, %.*s, %ld)", model_sql.length(), model_sql.ptr(),
          definition.input_expr_.length(), definition.input_expr_.ptr(), definition.dimension_));
    } else {
      const ObString config = definition.config_expr_.empty() ? ObString("JSON_OBJECT()") : definition.config_expr_;
      OZ(normalized.append_fmt("AI_COMPLETE(%.*s, %.*s, JSON_SET(COALESCE(%.*s, JSON_OBJECT()), '$.system_prompt', '%.*s'))",
          model_sql.length(), model_sql.ptr(), definition.input_expr_.length(), definition.input_expr_.ptr(),
          config.length(), config.ptr(), definition.system_prompt_.length(), definition.system_prompt_.ptr()));
    }
    definition.normalized_expr_ = normalized.string();
    const bool reuse_hidden = original != nullptr && original->is_ai_column();
    if (reuse_hidden) {
      AiColumnDefinition old;
      OZ(old.parse(*allocator_, original->get_ai_column_definition()));
      definition.state_column_id_ = old.state_column_id_;
      definition.request_column_id_ = old.request_column_id_;
      definition.result_spec_column_id_ = old.result_spec_column_id_;
    } else {
      definition.state_column_id_ = ++max_column_id;
      definition.request_column_id_ = ++max_column_id;
      definition.result_spec_column_id_ = ++max_column_id;
    }
    for (int i = 0; OB_SUCC(ret) && !reuse_hidden && i < 3; ++i) {
      ObColumnSchemaV2 physical;
      physical.set_table_id(table.get_table_id());
      physical.set_column_id(i == 0 ? definition.state_column_id_ :
          (i == 1 ? definition.request_column_id_ : definition.result_spec_column_id_));
      physical.set_is_hidden(true);
      physical.set_nullable(i != 0);
      ObSqlString name;
      OZ(name.append_fmt("__ai_%lu_%s", column.get_column_id(), i == 0 ? "state" : (i == 1 ? "request" : "result_spec")));
      OZ(physical.set_column_name(name.string()));
      if (table.get_column_schema(name.string()) != nullptr) { ret = OB_ERR_COLUMN_DUPLICATE; }
      if (i == 0) {
        physical.set_data_type(ObIntType);
        ObObj zero;
        zero.set_int(0);
        OZ(physical.set_cur_default_value(zero, false));
        OZ(physical.set_orig_default_value(zero));
      } else {
        physical.set_data_type(ObVarcharType);
        physical.set_data_length(64);
        physical.set_collation_type(CS_TYPE_UTF8MB4_BIN);
        physical.set_charset_type(CHARSET_UTF8MB4);
      }
      OZ(hidden.push_back(physical));
    }
    ObString serialized;
    OZ(definition.serialize(*allocator_, serialized));
    ObSqlString type_spec;
    OZ(type_spec.append_fmt("%d:%d:%d:%d:%d", column.get_data_type(), column.get_data_length(),
        column.get_data_precision(), column.get_data_scale(), column.get_collation_type()));
    definition.definition_version_ = murmurhash(serialized.ptr(), serialized.length(),
        murmurhash(type_spec.ptr(), type_spec.length(), 0));
    OZ(definition.serialize(*allocator_, serialized));
    OZ(column.set_ai_column_definition(serialized));
  }
  return ret;
}

int ObDDLResolver::finalize_ai_columns(ObTableSchema &table, uint64_t &max_column_id)
{
  int ret = OB_SUCCESS;
  const int64_t count = table.get_column_count();
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    ObColumnSchemaV2 *column = table.get_column_schema_by_idx(i);
    if (column == nullptr) { ret = OB_ERR_UNEXPECTED; }
    else if (column->is_ai_column()) {
      ObSEArray<ObColumnSchemaV2, 3> hidden;
      OZ(finalize_ai_column(*column, table, max_column_id, hidden));
      for (int64_t j = 0; OB_SUCC(ret) && j < hidden.count(); ++j) { ret = table.add_column(hidden.at(j)); }
    }
  }
  return ret;
}
}
}
