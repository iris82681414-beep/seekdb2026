/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SERVER
#include "observer/ai_service/ob_ai_column_service.h"
#include "observer/omt/ob_ai_service.h"
#include "observer/ob_inner_sql_result.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_ai_column_schema.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "sql/engine/expr/ob_expr_uuid.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "sql/engine/expr/ob_expr_column_conv.h"
#include "lib/ob_check_macros.h"
#include "lib/worker.h"
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace oceanbase
{
namespace observer
{
using namespace common;
using common::sqlclient::ObMySQLResult;
using namespace share;
using namespace share::schema;
namespace
{
ObString column_view(const std::string &value) { return ObString(value.size(), value.data()); }
int column_text(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("CONVERT(X'"));
  for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    OZ(sql.append_fmt("%02x", static_cast<unsigned char>(value.ptr()[i])));
  }
  OZ(sql.append("' USING utf8mb4)"));
  return ret;
}
int column_identifier(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  OZ(sql.append("`"));
  for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    if (value.ptr()[i] == '`') { OZ(sql.append("`")); }
    OZ(sql.append(ObString(1, value.ptr() + i)));
  }
  OZ(sql.append("`"));
  return ret;
}
int column_key_literal(ObIAllocator &allocator, const ObObj &value,
    const ObColumnSchemaV2 &column, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  if (value.is_string_type()) {
    const bool binary = column.get_collation_type() == CS_TYPE_BINARY;
    OZ(sql.append(binary ? "X'" : "CONVERT(X'"));
    const ObString bytes = value.get_string();
    for (int32_t i = 0; OB_SUCC(ret) && i < bytes.length(); ++i) {
      OZ(sql.append_fmt("%02x", static_cast<unsigned char>(bytes.ptr()[i])));
    }
    if (binary) { OZ(sql.append("'")); }
    else {
      OZ(sql.append_fmt("' USING %s)", ObCharset::charset_name(
          ObCharset::charset_type_by_coll(column.get_collation_type()))));
    }
  } else if (value.is_timestamp() && value.get_timestamp() >= 0) {
    OZ(sql.append_fmt("FROM_UNIXTIME(%ld.%06ld)", value.get_timestamp() / 1000000, value.get_timestamp() % 1000000));
  } else {
    char *buffer = nullptr;
    int64_t capacity = 0;
    int64_t position = 0;
    OZ(value.print_sql_literal(buffer, capacity, position, allocator, ObObjPrintParams()));
    OZ(sql.append(ObString(position, buffer)));
  }
  return ret;
}
int column_string(ObMySQLResult &result, const char *name, std::string &value)
{
  int ret = OB_SUCCESS;
  ObString text;
  ObObj object;
  OZ(result.get_obj(name, object));
  ObArenaAllocator allocator("AiColumnRead");
  if (OB_SUCC(ret) && object.is_lob_storage()) {
    ObInnerSQLResult *native = dynamic_cast<ObInnerSQLResult *>(&result);
    if (native == nullptr) { ret = OB_NOT_SUPPORTED; }
    else { ret = sql::ObTextStringHelper::read_real_string_data(native->result_set().get_exec_context(), &allocator, object, text); }
  } else if (OB_SUCC(ret)) { ret = object.get_string(text); }
  if (OB_SUCC(ret)) { value.assign(text.ptr(), text.length()); }
  return ret;
}
struct ColumnTask
{
  uint64_t table_id = 0;
  uint64_t column_id = 0;
  uint64_t definition_version = 0;
  int64_t retry_count = 0;
  int64_t state = 0;
  bool result_is_null = false;
  std::string row_hash;
  std::string predicate;
  std::string request;
  std::string owner;
  std::string result;
};
int column_task_fields(ObMySQLResult &result, ColumnTask &task)
{
  int ret = OB_SUCCESS;
  int64_t table = 0;
  int64_t column = 0;
  OZ(result.get_int("table_id", table));
  OZ(result.get_int("column_id", column));
  OZ(result.get_uint("definition_version", task.definition_version));
  OZ(result.get_int("retry_count", task.retry_count));
  OZ(result.get_int("state", task.state));
  OZ(column_string(result, "rowkey_hash", task.row_hash));
  OZ(column_string(result, "rowkey_predicate", task.predicate));
  OZ(column_string(result, "request_id", task.request));
  if (OB_SUCC(ret) && (table <= 0 || column <= 0 || task.row_hash.size() != 64 || task.request.empty())) {
    ret = OB_INVALID_DATA;
  }
  task.table_id = table;
  task.column_id = column;
  return ret;
}
int column_task_key(ObSqlString &sql, const ColumnTask &task, bool group, bool ownership)
{
  int ret = OB_SUCCESS;
  OZ(sql.append_fmt("table_id=%lu AND rowkey_hash=", task.table_id));
  OZ(column_text(sql, column_view(task.row_hash)));
  OZ(sql.append(" AND request_id="));
  OZ(column_text(sql, column_view(task.request)));
  if (!group) {
    OZ(sql.append_fmt(" AND column_id=%lu AND definition_version=%lu", task.column_id, task.definition_version));
  }
  if (ownership) {
    OZ(sql.append(" AND lease_owner="));
    OZ(column_text(sql, column_view(task.owner)));
  }
  return ret;
}
bool column_terminal_write_error(int error)
{
  return error == OB_ERR_PRIMARY_KEY_DUPLICATE || error == OB_ERR_CHECK_CONSTRAINT_VIOLATED ||
      error == OB_ERR_NO_REFERENCED_ROW || error == OB_ERR_DATA_TOO_LONG ||
      error == OB_DATA_OUT_OF_RANGE || error == OB_ERR_TRUNCATED_WRONG_VALUE ||
      error == OB_ERR_TRUNCATED_WRONG_VALUE_FOR_FIELD || error == OB_ERR_WARN_DATA_OUT_OF_RANGE;
}
class ColumnTransaction
{
public:
  ColumnTransaction() : session_(nullptr), saved_(false), saved_mode_(0) {}
  ~ColumnTransaction() { end(false); }
  int start()
  {
    int ret = OB_SUCCESS;
    OZ(transaction_.start(GCTX.sql_proxy_));
    if (OB_SUCC(ret)) {
      session_ = query::ObInnerSQLConnectionAccess::get_session(transaction_.get_connection());
      if (session_ == nullptr) { ret = OB_NOT_INIT; }
      else {
        saved_ = session_->is_ai_column_internal_write();
        saved_mode_ = session_->get_sql_mode();
        session_->set_ai_column_internal_write(true);
        session_->set_sql_mode(saved_mode_ | SMO_STRICT_ALL_TABLES);
      }
    }
    return ret;
  }
  int end(bool commit)
  {
    if (session_ != nullptr) {
      session_->set_ai_column_internal_write(saved_);
      session_->set_sql_mode(saved_mode_);
      session_ = nullptr;
    }
    return transaction_.is_started() ? transaction_.end(commit) : OB_SUCCESS;
  }
  ObMySQLTransaction transaction_;
private:
  sql::ObSQLSessionInfo *session_;
  bool saved_;
  ObSQLMode saved_mode_;
};
int column_schema(const ColumnTask &task, ObSchemaGetterGuard &guard,
                  const ObTableSchema *&table, const ObColumnSchemaV2 *&column)
{
  int ret = OB_SUCCESS;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(task.table_id, table));
  if (OB_SUCC(ret) && (table == nullptr || !table->is_user_table() || table->is_in_recyclebin())) {
    ret = OB_TABLE_NOT_EXIST;
  }
  if (OB_SUCC(ret)) {
    column = table->get_column_schema(task.column_id);
    if (column == nullptr || !column->is_ai_column() || column->is_unused()) { ret = OB_ERR_BAD_FIELD_ERROR; }
  }
  return ret;
}
int column_target(ObSchemaGetterGuard &guard, const ObTableSchema &table, ObSqlString &target)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database = nullptr;
  OZ(guard.get_database_schema(table.get_database_id(), database));
  if (OB_SUCC(ret) && database == nullptr) { ret = OB_ERR_BAD_DATABASE; }
  if (OB_SUCC(ret)) { ret = column_identifier(target, database->get_database_name_str()); }
  OZ(target.append("."));
  OZ(column_identifier(target, table.get_table_name_str()));
  return ret;
}
int column_request_guard(ObSqlString &sql, const ObTableSchema &table,
    const AiColumnDefinition &definition, const ColumnTask &task)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *request = table.get_column_schema(definition.request_column_id_);
  const ObColumnSchemaV2 *spec = table.get_column_schema(definition.result_spec_column_id_);
  if (request == nullptr || spec == nullptr) { return OB_SCHEMA_ERROR; }
  OZ(column_identifier(sql, request->get_column_name_str()));
  OZ(sql.append("="));
  OZ(column_text(sql, column_view(task.request)));
  OZ(sql.append(" AND "));
  OZ(column_identifier(sql, spec->get_column_name_str()));
  OZ(sql.append_fmt("='%lu'", task.definition_version));
  return ret;
}
std::string column_trim(const ObString &input)
{
  std::string result(input.ptr(), input.length());
  const auto blank = [](char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; };
  size_t begin = 0;
  while (begin < result.size() && blank(result[begin])) { ++begin; }
  size_t end = result.size();
  while (end > begin && blank(result[end - 1])) { --end; }
  return result.substr(begin, end - begin);
}
int column_validate_result(ObIAllocator &allocator, const ObColumnSchemaV2 &column,
                            const ObString &result)
{
  int ret = OB_SUCCESS;
  if (result.length() > 16 * 1024 * 1024) { return OB_SIZE_OVERFLOW; }
  const ObObjType type = column.get_data_type();
  if (column.is_json()) {
    ObIJsonBase *json = nullptr;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, result,
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
  } else if (ob_is_integer_type(type)) {
    const std::string value = column_trim(result);
    if (value.empty()) { return OB_INVALID_DATA; }
    const char *begin = value.data() + (value[0] == '+' ? 1 : 0);
    const char *end = value.data() + value.size();
    int bits = 64;
    if (type == ObTinyIntType || type == ObUTinyIntType) { bits = 8; }
    else if (type == ObSmallIntType || type == ObUSmallIntType) { bits = 16; }
    else if (type == ObMediumIntType || type == ObUMediumIntType) { bits = 24; }
    else if (type == ObInt32Type || type == ObUInt32Type) { bits = 32; }
    if (ob_is_uint_tc(type)) {
      uint64_t number = 0;
      const auto parsed = std::from_chars(begin, end, number);
      if (parsed.ec != std::errc() || parsed.ptr != end ||
          (bits < 64 && number > ((uint64_t(1) << bits) - 1))) { ret = OB_INVALID_DATA; }
    } else {
      int64_t number = 0;
      const auto parsed = std::from_chars(begin, end, number);
      if (parsed.ec != std::errc() || parsed.ptr != end ||
          (bits < 64 && (number < -(int64_t(1) << (bits - 1)) ||
                        number > (int64_t(1) << (bits - 1)) - 1))) { ret = OB_INVALID_DATA; }
    }
  } else if (ob_is_numeric_type(type) || ob_is_decimal_int(type)) {
    const std::string value = column_trim(result);
    if (value.empty()) { return OB_INVALID_DATA; }
    char *end = nullptr;
    errno = 0;
    const double number = std::strtod(value.c_str(), &end);
    if (end != value.c_str() + value.size() || errno == ERANGE || !std::isfinite(number)) { ret = OB_INVALID_DATA; }
  } else if (ob_is_string_type(type) || ob_is_text_tc(type)) {
    int64_t valid = 0;
    OZ(ObCharset::well_formed_len(CS_TYPE_UTF8MB4_BIN, result.ptr(), result.length(), valid));
    if (OB_SUCC(ret) && valid != result.length()) { ret = OB_INVALID_DATA; }
    if (OB_SUCC(ret) && column.get_data_length() > 0) {
      const uint64_t length = column.get_collation_type() == CS_TYPE_BINARY || ob_is_text_tc(type) ?
          result.length() : ObCharset::strlen_char(CS_TYPE_UTF8MB4_BIN, result.ptr(), result.length());
      if (length > column.get_data_length()) { ret = OB_ERR_DATA_TOO_LONG; }
    }
  }
  if (OB_SUCC(ret) && !column.is_json() && !column.is_collection()) {
    // Use the same strict conversion and accuracy checks as ordinary writes,
    // including charset representability, decimal precision and date validity.
    ObObj source;
    source.set_varchar(result);
    source.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    ObObj converted;
    sql::ObExprResType target;
    target.set_type(type);
    target.set_collation_type(column.get_collation_type());
    target.set_accuracy(column.get_accuracy());
    ObTimeZoneInfo timezone;
    ObDataTypeCastParams params(&timezone);
    ObCastCtx context(&allocator, &params, CM_STRICT_MODE | CM_COLUMN_CONVERT |
        CM_INSERT_UPDATE_SCOPE | CM_NO_ZERO_DATE | CM_NO_ZERO_IN_DATE, column.get_collation_type());
    OZ(sql::ObExprColumnConv::convert_skip_null_check(converted, source, target, true,
        context, &column.get_extended_type_info()));
    if (OB_SUCC(ret) && context.warning_ != OB_SUCCESS) { ret = context.warning_; }
  }
  return ret;
}
struct ColumnBackfill
{
  std::string signature;
  std::string cursor;
  bool done = false;
};

int column_backfill_table(ObSchemaGetterGuard &guard, const ObTableSchema &table,
    const std::vector<const ObColumnSchemaV2 *> &columns,
    const std::vector<AiColumnDefinition *> &definitions, ColumnBackfill &progress)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnFill");
  ObSqlString target;
  ObSqlString keys;
  ObSqlString stale;
  OZ(column_target(guard, table, target));
  const ObRowkeyInfo &rowkeys = table.get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkeys.get_size(); ++i) {
    const ObColumnSchemaV2 *column = table.get_column_schema(rowkeys.get_column(i)->column_id_);
    if (column == nullptr) { ret = OB_SCHEMA_ERROR; }
    else {
      if (i != 0) { OZ(keys.append(",")); }
      OZ(column_identifier(keys, column->get_column_name_str()));
    }
  }
  if (OB_SUCC(ret) && rowkeys.get_size() == 0) { ret = OB_NOT_SUPPORTED; }
  ObSqlString select;
  OZ(select.append_fmt("SELECT %s", keys.ptr()));
  for (size_t i = 0; OB_SUCC(ret) && i < definitions.size(); ++i) {
    const AiColumnDefinition &definition = *definitions[i];
    const ObColumnSchemaV2 *request = table.get_column_schema(definition.request_column_id_);
    const ObColumnSchemaV2 *spec = table.get_column_schema(definition.result_spec_column_id_);
    if (request == nullptr || spec == nullptr) { ret = OB_SCHEMA_ERROR; }
    else {
      OZ(select.append(","));
      OZ(column_identifier(select, request->get_column_name_str()));
      OZ(select.append(","));
      OZ(column_identifier(select, spec->get_column_name_str()));
      if (i != 0) { OZ(stale.append(" OR ")); }
      OZ(stale.append("("));
      OZ(column_identifier(stale, request->get_column_name_str()));
      OZ(stale.append(" IS NULL OR "));
      OZ(column_identifier(stale, spec->get_column_name_str()));
      OZ(stale.append(" IS NULL OR "));
      OZ(column_identifier(stale, spec->get_column_name_str()));
      OZ(stale.append_fmt("<>'%lu')", definition.definition_version_));
    }
  }
  OZ(select.append_fmt(" FROM %s WHERE (%s)", target.ptr(), stale.ptr()));
  if (!progress.cursor.empty()) {
    OZ(select.append_fmt(" AND (%s)>(%s)", keys.ptr(), progress.cursor.c_str()));
  }
  OZ(select.append_fmt(" ORDER BY %s LIMIT 32 FOR UPDATE", keys.ptr()));
  struct FillRow { std::string predicate; std::string cursor; std::vector<size_t> dirty; };
  std::vector<FillRow> rows;
  ColumnTransaction transaction;
  OZ(transaction.start());
  {
    ObMySQLProxy::MySQLResult result;
    OZ(transaction.transaction_.read(result, select.ptr()));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret)) {
      ret = result.get_result()->next();
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; break; }
      if (OB_SUCC(ret)) {
        FillRow row;
        ObSqlString predicate;
        ObSqlString cursor;
        for (int64_t i = 0; OB_SUCC(ret) && i < rowkeys.get_size(); ++i) {
          const ObColumnSchemaV2 *column = table.get_column_schema(rowkeys.get_column(i)->column_id_);
          ObObj value;
          OZ(result.get_result()->get_obj(i, value));
          if (OB_SUCC(ret) && value.is_null()) { ret = OB_ERR_NULL_VALUE; }
          ObSqlString literal;
          OZ(column_key_literal(allocator, value, *column, literal));
          if (i != 0) { OZ(predicate.append(" AND ")); OZ(cursor.append(",")); }
          OZ(column_identifier(predicate, column->get_column_name_str()));
          OZ(predicate.append(" = "));
          OZ(predicate.append(literal.string()));
          OZ(cursor.append(literal.string()));
        }
        for (size_t i = 0; OB_SUCC(ret) && i < definitions.size(); ++i) {
          ObObj request;
          ObObj spec;
          const int64_t offset = rowkeys.get_size() + 2 * i;
          OZ(result.get_result()->get_obj(offset, request));
          OZ(result.get_result()->get_obj(offset + 1, spec));
          const std::string version = std::to_string(definitions[i]->definition_version_);
          if (OB_SUCC(ret) && (request.is_null() || spec.is_null() ||
              spec.get_string() != column_view(version))) { row.dirty.push_back(i); }
        }
        if (OB_SUCC(ret)) {
          row.predicate.assign(predicate.ptr(), predicate.length());
          row.cursor.assign(cursor.ptr(), cursor.length());
          rows.push_back(std::move(row));
        }
      }
    }
  }
  for (size_t n = 0; OB_SUCC(ret) && n < rows.size(); ++n) {
    const FillRow &row = rows[n];
    char request[sql::UuidCommon::LENGTH_UUID];
    OZ(sql::ObExprUuid::gen_server_uuid(request, sizeof(request)));
    const ObString request_id(sizeof(request), request);
    ObSqlString update;
    OZ(update.append_fmt("UPDATE %s SET ", target.ptr()));
    for (size_t j = 0; OB_SUCC(ret) && j < row.dirty.size(); ++j) {
      const size_t i = row.dirty[j];
      const AiColumnDefinition &definition = *definitions[i];
      const uint64_t ids[] = {columns[i]->get_column_id(), definition.state_column_id_,
          definition.request_column_id_, definition.result_spec_column_id_};
      for (size_t k = 0; OB_SUCC(ret) && k < 4; ++k) {
        const ObColumnSchemaV2 *column = table.get_column_schema(ids[k]);
        if (column == nullptr) { ret = OB_SCHEMA_ERROR; }
        else {
          if (j != 0 || k != 0) { OZ(update.append(",")); }
          OZ(column_identifier(update, column->get_column_name_str()));
          OZ(update.append("="));
          if (k == 0) { OZ(update.append("NULL")); }
          else if (k == 1) { OZ(update.append("0")); }
          else if (k == 2) { OZ(column_text(update, request_id)); }
          else { OZ(update.append_fmt("'%lu'", definition.definition_version_)); }
        }
      }
    }
    OZ(update.append_fmt(" WHERE %s", row.predicate.c_str()));
    int64_t affected = 0;
    if (OB_SUCC(ret) && !row.dirty.empty()) { ret = transaction.transaction_.write(update.ptr(), affected); }
    for (size_t j = 0; OB_SUCC(ret) && affected > 0 && j < row.dirty.size(); ++j) {
      const size_t i = row.dirty[j];
      ObSqlString task;
      OZ(task.append_fmt("REPLACE INTO oceanbase.__all_ai_column_task(table_id,rowkey_hash,column_id,request_id,definition_version,rowkey_predicate,state,retry_count,eligible_ts) VALUES(%lu,SHA2(", table.get_table_id()));
      OZ(column_text(task, column_view(row.predicate)));
      OZ(task.append_fmt(",256),%lu,", columns[i]->get_column_id()));
      OZ(column_text(task, request_id));
      OZ(task.append_fmt(",%lu,", definitions[i]->definition_version_));
      OZ(column_text(task, column_view(row.predicate)));
      OZ(task.append(",0,0,NOW(6))"));
      int64_t inserted = 0;
      OZ(transaction.transaction_.write(task.ptr(), inserted));
    }
  }
  const int end = transaction.end(OB_SUCC(ret));
  if (OB_SUCC(ret)) { ret = end; }
  if (OB_SUCC(ret)) {
    if (rows.empty()) { progress.done = true; }
    else { progress.cursor = rows.back().cursor; }
  }
  return ret;
}
}

struct AiColumnService::Impl
{
  std::string boot;
  std::mutex mutex;
  std::map<uint64_t, int64_t> active;
  bool reclaimed = false;
  std::map<uint64_t, ColumnBackfill> backfills;

  bool acquire(uint64_t endpoint, int64_t limit)
  {
    std::lock_guard<std::mutex> lock(mutex);
    int64_t &count = active[endpoint];
    if (count >= limit) { return false; }
    ++count;
    return true;
  }
  void release(uint64_t endpoint)
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = active.find(endpoint);
    if (found != active.end() && --found->second == 0) { active.erase(found); }
  }
  int claim(const std::string &owner, ColumnTask &task, bool &found)
  {
    int ret = OB_SUCCESS;
    found = false;
    ObSqlString select;
    OZ(select.append("SELECT table_id,rowkey_hash,column_id,request_id,definition_version,rowkey_predicate,retry_count,state FROM oceanbase.__all_ai_column_task WHERE ((state IN(0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_expire_ts<NOW(6))) ORDER BY eligible_ts,table_id,column_id LIMIT 1"));
    {
      ObMySQLProxy::MySQLResult result;
      OZ(GCTX.sql_proxy_->read(result, select.ptr()));
      if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      if (OB_SUCC(ret)) {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) { ret = column_task_fields(*result.get_result(), task); found = OB_SUCC(ret); }
      }
    }
    if (OB_SUCC(ret) && found) {
      task.owner = owner;
      ObSqlString sql;
      OZ(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=1,lease_owner="));
      OZ(column_text(sql, column_view(owner)));
      OZ(sql.append(",lease_expire_ts=DATE_ADD(NOW(6),INTERVAL 30 SECOND) WHERE "));
      OZ(column_task_key(sql, task, false, false));
      OZ(sql.append(" AND ((state IN(0,3) AND eligible_ts<=NOW(6)) OR (state=1 AND lease_expire_ts<NOW(6)))"));
      int64_t affected = 0;
      OZ(GCTX.sql_proxy_->write(sql.ptr(), affected));
      found = OB_SUCC(ret) && affected == 1;
    }
    return ret;
  }
  int discard(const ColumnTask &task)
  {
    int ret = OB_SUCCESS;
    ObSqlString sql;
    OZ(sql.append("DELETE FROM oceanbase.__all_ai_column_task WHERE "));
    OZ(column_task_key(sql, task, false, true));
    int64_t affected = 0;
    OZ(GCTX.sql_proxy_->write(sql.ptr(), affected));
    return ret;
  }
  int input(const ColumnTask &task, ObSchemaGetterGuard &guard, const ObTableSchema &table,
            const AiColumnDefinition &definition, std::string &prompt, std::string &config,
            bool &is_null, bool &current)
  {
    int ret = OB_SUCCESS;
    current = false;
    is_null = false;
    ObSqlString target;
    OZ(column_target(guard, table, target));
    ObSqlString sql;
    const ObString config_expr = definition.config_expr_.empty() ? ObString("NULL") : definition.config_expr_;
    OZ(sql.append_fmt("SELECT CAST((%.*s) AS CHAR CHARACTER SET utf8mb4) AS ai_input,CAST((%.*s) AS CHAR CHARACTER SET utf8mb4) AS ai_config FROM %s WHERE (%s) AND ",
        definition.input_expr_.length(), definition.input_expr_.ptr(), config_expr.length(), config_expr.ptr(),
        target.ptr(), task.predicate.c_str()));
    OZ(column_request_guard(sql, table, definition, task));
    ColumnTransaction transaction;
    OZ(transaction.start());
    {
      ObMySQLProxy::MySQLResult result;
      OZ(transaction.transaction_.read(result, sql.ptr()));
      if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      if (OB_SUCC(ret)) {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) {
          current = true;
          ObObj input;
          OZ(result.get_result()->get_obj(static_cast<int64_t>(0), input));
          is_null = input.is_null();
          if (OB_SUCC(ret) && !is_null) { ret = column_string(*result.get_result(), "ai_input", prompt); }
          ObObj options;
          OZ(result.get_result()->get_obj(static_cast<int64_t>(1), options));
          if (OB_SUCC(ret) && !options.is_null()) { ret = column_string(*result.get_result(), "ai_config", config); }
        }
      }
    }
    const int end = transaction.end(OB_SUCC(ret));
    if (OB_SUCC(ret)) { ret = end; }
    return ret;
  }
  int process(ColumnTask &task);
  int publish(const ColumnTask &task);
  int reconcile();
  int backfill();
};

int AiColumnService::Impl::process(ColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnHTTP");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  const ObColumnSchemaV2 *column = nullptr;
  AiColumnDefinition definition;
  ret = column_schema(task, guard, table, column);
  if (OB_SUCC(ret)) { ret = definition.parse(allocator, column->get_ai_column_definition()); }
  if (ret == OB_TABLE_NOT_EXIST || ret == OB_ERR_BAD_FIELD_ERROR) { return discard(task); }
  if (OB_FAIL(ret)) { return ret; }
  if (definition.definition_version_ != task.definition_version) { return discard(task); }
  std::string prompt;
  std::string config_text;
  bool is_null = false;
  bool current = false;
  ret = input(task, guard, *table, definition, prompt, config_text, is_null, current);
  if (OB_SUCC(ret) && !current) { return discard(task); }
  const bool embedding = definition.dimension_ > 0;
  bool null_result = embedding && is_null;
  ObJsonObject *config = nullptr;
  if (OB_SUCC(ret) && !config_text.empty()) {
    ObIJsonBase *json = nullptr;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, column_view(config_text),
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
    if (OB_SUCC(ret) && (json == nullptr || json->json_type() != ObJsonNodeType::J_OBJECT)) { ret = OB_INVALID_ARGUMENT; }
    if (OB_SUCC(ret)) { config = static_cast<ObJsonObject *>(json); }
  }
  if (OB_SUCC(ret) && is_null && !embedding) { ret = OB_INVALID_ARGUMENT; }
  ObString model_result;
  if (OB_SUCC(ret) && !is_null && !embedding && definition.input_is_json_) {
    ObIJsonBase *json = nullptr;
    OZ(ObJsonBaseFactory::get_json_base(&allocator, column_view(prompt),
        ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, json));
    if (OB_SUCC(ret) && (json == nullptr || json->json_type() != ObJsonNodeType::J_OBJECT ||
        !ObAIFuncPromptObjectUtils::is_valid_prompt_object(static_cast<ObJsonObject *>(json)))) {
      ret = OB_INVALID_ARGUMENT;
    }
    ObString expanded;
    if (OB_SUCC(ret)) {
      ret = ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator,
          static_cast<ObJsonObject *>(json), expanded);
    }
    if (OB_SUCC(ret)) { prompt.assign(expanded.ptr(), expanded.length()); }
  }
  if (OB_SUCC(ret) && !null_result) {
    query::ObIAiEndpointResolver *resolver = server_service<query::ObIAiEndpointResolver>();
    ObAiModelEndpointInfo endpoint;
    ObAIFuncExprInfo *info = nullptr;
    if (resolver == nullptr) { ret = OB_NOT_INIT; }
    OZ(ObAIFuncUtils::get_ai_func_info(allocator, definition.model_, info));
    OZ(resolver->resolve_by_model_name(definition.model_, allocator, endpoint, false));
    bool acquired = false;
    if (OB_SUCC(ret)) {
      acquired = acquire(endpoint.get_endpoint_id(), endpoint.get_max_concurrency());
      if (!acquired) {
        ObSqlString sql;
        OZ(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=0,eligible_ts=DATE_ADD(NOW(6),INTERVAL 100000 MICROSECOND),lease_owner=NULL,lease_expire_ts=NULL WHERE "));
        OZ(column_task_key(sql, task, false, true));
        int64_t affected = 0;
        OZ(GCTX.sql_proxy_->write(sql.ptr(), affected));
        return OB_SUCC(ret) ? OB_EAGAIN : ret;
      }
    }
    if (OB_SUCC(ret)) {
      // The task queue owns retries; one dispatch must issue only one request.
      ObAIFuncModel model(allocator, *info, endpoint, false /* enable_retry */);
      ObString content = column_view(prompt);
      if (embedding) {
        ObJsonObject dimensions(&allocator);
        ObJsonInt size(definition.dimension_);
        OZ(dimensions.add("dimensions", &size));
        OZ(model.call_dense_embedding(content, &dimensions, model_result));
      } else {
        OZ(model.call_completion(content, config, model_result, definition.system_prompt_));
      }
      OZ(column_validate_result(allocator, *column, model_result));
    }
    if (acquired) { release(endpoint.get_endpoint_id()); }
  }
  // Persist attempt outcome before grouped publication. No row/task lock or
  // SQL connection survives across the upstream request above.
  const int attempt_error = ret;
  ret = OB_SUCCESS;
  ObSqlString sql;
  const bool retry = attempt_error != OB_SUCCESS && task.retry_count < definition.max_retries_;
  if (retry) {
    OZ(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=3,retry_count=%ld,eligible_ts=DATE_ADD(NOW(6),INTERVAL 1 SECOND),last_error=%d,lease_owner=NULL,lease_expire_ts=NULL WHERE ",
        task.retry_count + 1, attempt_error));
  } else {
    OZ(sql.append_fmt("UPDATE oceanbase.__all_ai_column_task SET state=%d,last_error=%d,result_is_null=%d,result=",
        attempt_error == OB_SUCCESS ? 4 : 5, attempt_error, null_result ? 1 : 0));
    if (attempt_error == OB_SUCCESS && !null_result) { OZ(column_text(sql, model_result)); }
    else { OZ(sql.append("NULL")); }
    OZ(sql.append(" WHERE "));
  }
  OZ(column_task_key(sql, task, false, true));
  OZ(sql.append(" AND state=1"));
  int64_t affected = 0;
  OZ(GCTX.sql_proxy_->write(sql.ptr(), affected));
  if (OB_SUCC(ret) && affected == 1 && !retry) { ret = publish(task); }
  return ret;
}

int AiColumnService::Impl::publish(const ColumnTask &task)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnT2");
  ObSchemaGetterGuard guard;
  const ObTableSchema *table = nullptr;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schema(task.table_id, table));
  if (OB_SUCC(ret) && (table == nullptr || !table->is_user_table() || table->is_in_recyclebin())) {
    ObSqlString cleanup;
    ret = cleanup.append("DELETE FROM oceanbase.__all_ai_column_task WHERE ");
    OZ(column_task_key(cleanup, task, true, false));
    int64_t affected = 0;
    OZ(GCTX.sql_proxy_->write(cleanup.ptr(), affected));
    return ret;
  }
  ObSqlString target;
  OZ(column_target(guard, *table, target));
  ColumnTransaction transaction;
  OZ(transaction.start());
  bool row_exists = false;
  {
    // T1 locks the business row before touching task rows. Keep the same order
    // in T2, including groups completed by different workers.
    ObSqlString lock;
    OZ(lock.append_fmt("SELECT 1 AS ai_exists FROM %s WHERE (%s) FOR UPDATE", target.ptr(), task.predicate.c_str()));
    ObMySQLProxy::MySQLResult result;
    OZ(transaction.transaction_.read(result, lock.ptr()));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    if (OB_SUCC(ret)) {
      ret = result.get_result()->next();
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
      else if (OB_SUCC(ret)) { row_exists = true; }
    }
  }
  std::vector<ColumnTask> group;
  bool terminal = true;
  {
    ObSqlString select;
    OZ(select.append("SELECT table_id,rowkey_hash,column_id,request_id,definition_version,rowkey_predicate,retry_count,state,result_is_null,result FROM oceanbase.__all_ai_column_task WHERE "));
    OZ(column_task_key(select, task, true, false));
    OZ(select.append(" ORDER BY column_id FOR UPDATE"));
    ObMySQLProxy::MySQLResult result;
    OZ(transaction.transaction_.read(result, select.ptr()));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret)) {
      ret = result.get_result()->next();
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; break; }
      if (OB_SUCC(ret)) {
        ColumnTask member;
        OZ(column_task_fields(*result.get_result(), member));
        terminal = terminal && (member.state == 4 || member.state == 5);
        int64_t null_value = 0;
        OZ(result.get_result()->get_int("result_is_null", null_value));
        member.result_is_null = null_value != 0;
        if (OB_SUCC(ret) && member.state == 4 && !member.result_is_null) {
          ret = column_string(*result.get_result(), "result", member.result);
        }
        if (OB_SUCC(ret)) { group.push_back(std::move(member)); }
      }
    }
  }
  if (OB_SUCC(ret) && !terminal) { return transaction.end(false); }
  ObSqlString assignments;
  ObSqlString guards;
  struct Publication { std::string ready; std::string failed; };
  std::vector<Publication> publications;
  for (size_t i = 0; OB_SUCC(ret) && row_exists && i < group.size(); ++i) {
    const ColumnTask &member = group[i];
    const ObColumnSchemaV2 *result_column = table->get_column_schema(member.column_id);
    if (result_column == nullptr || !result_column->is_ai_column() || result_column->is_unused()) { continue; }
    AiColumnDefinition definition;
    OZ(definition.parse(allocator, result_column->get_ai_column_definition()));
    if (OB_SUCC(ret) && definition.definition_version_ != member.definition_version) { continue; }
    bool matches = false;
    {
      ObSqlString select;
      OZ(select.append_fmt("SELECT 1 AS ai_current FROM %s WHERE (%s) AND ", target.ptr(), member.predicate.c_str()));
      OZ(column_request_guard(select, *table, definition, member));
      ObMySQLProxy::MySQLResult result;
      OZ(transaction.transaction_.read(result, select.ptr()));
      if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
      if (OB_SUCC(ret)) {
        ret = result.get_result()->next();
        if (ret == OB_ITER_END) { ret = OB_SUCCESS; }
        else if (OB_SUCC(ret)) { matches = true; }
      }
    }
    if (!matches) { continue; }
    const ObColumnSchemaV2 *state = table->get_column_schema(definition.state_column_id_);
    if (state == nullptr) { ret = OB_SCHEMA_ERROR; }
    else {
      ObSqlString ready;
      ObSqlString failed;
      OZ(column_identifier(ready, result_column->get_column_name_str()));
      OZ(ready.append("="));
      if (member.state == 4 && !member.result_is_null) { OZ(column_text(ready, column_view(member.result))); }
      else { OZ(ready.append("NULL")); }
      OZ(ready.append(","));
      OZ(column_identifier(ready, state->get_column_name_str()));
      OZ(ready.append_fmt("=%d", member.state == 4 ? 1 : 2));
      OZ(column_identifier(failed, result_column->get_column_name_str()));
      OZ(failed.append("=NULL,"));
      OZ(column_identifier(failed, state->get_column_name_str()));
      OZ(failed.append("=2"));
      if (!assignments.empty()) { OZ(assignments.append(",")); }
      OZ(assignments.append(ready.string()));
      if (OB_SUCC(ret)) {
        publications.push_back({std::string(ready.ptr(), ready.length()),
            std::string(failed.ptr(), failed.length())});
      }
      if (!guards.empty()) { OZ(guards.append(" AND ")); }
      OZ(column_request_guard(guards, *table, definition, member));
    }
  }
  if (OB_SUCC(ret) && !assignments.empty()) {
    ObSqlString update;
    OZ(update.append_fmt("UPDATE %s SET %s WHERE (%s) AND %s", target.ptr(), assignments.ptr(), task.predicate.c_str(), guards.ptr()));
    int64_t affected = 0;
    OZ(transaction.transaction_.write(update.ptr(), affected));
    if (column_terminal_write_error(ret)) {
      // A model can return a valid value that violates a user index or
      // constraint. Retain valid siblings and finish the rejected column as
      // FAILED, while keeping every publication inside this one transaction.
      ret = OB_SUCCESS;
      transaction.transaction_.reset_last_error();
      for (size_t i = 0; OB_SUCC(ret) && i < publications.size(); ++i) {
        ObSqlString single;
        OZ(single.append_fmt("UPDATE %s SET %s WHERE (%s) AND %s", target.ptr(),
            publications[i].ready.c_str(), task.predicate.c_str(), guards.ptr()));
        OZ(transaction.transaction_.write(single.ptr(), affected));
        if (column_terminal_write_error(ret)) {
          ret = OB_SUCCESS;
          transaction.transaction_.reset_last_error();
          ObSqlString failed;
          OZ(failed.append_fmt("UPDATE %s SET %s WHERE (%s) AND %s", target.ptr(),
              publications[i].failed.c_str(), task.predicate.c_str(), guards.ptr()));
          OZ(transaction.transaction_.write(failed.ptr(), affected));
        }
      }
    }
  }
  if (OB_SUCC(ret) && !group.empty()) {
    ObSqlString cleanup;
    OZ(cleanup.append("DELETE FROM oceanbase.__all_ai_column_task WHERE "));
    OZ(column_task_key(cleanup, task, true, false));
    int64_t affected = 0;
    OZ(transaction.transaction_.write(cleanup.ptr(), affected));
  }
  const int end = transaction.end(OB_SUCC(ret));
  if (OB_SUCC(ret)) { ret = end; }
  return ret;
}

int AiColumnService::Impl::backfill()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObSEArray<const ObTableSchema *, 16> tables;
  OZ(GCTX.schema_service_->get_runtime_schema_guard(guard));
  OZ(guard.get_table_schemas_in_runtime(tables));
  for (int64_t n = 0; OB_SUCC(ret) && n < tables.count(); ++n) {
    const ObTableSchema *table = tables.at(n);
    if (table == nullptr || !table->is_user_table() || table->is_in_recyclebin()) { continue; }
    ObArenaAllocator allocator("AiColumnDefs");
    using DefinitionPtr = std::unique_ptr<AiColumnDefinition, void (*)(AiColumnDefinition *)>;
    std::vector<DefinitionPtr> owned;
    std::vector<AiColumnDefinition *> definitions;
    std::vector<const ObColumnSchemaV2 *> columns;
    std::string signature;
    for (int64_t i = 0; OB_SUCC(ret) && i < table->get_column_count(); ++i) {
      const ObColumnSchemaV2 *column = table->get_column_schema_by_idx(i);
      if (column != nullptr && column->is_ai_column() && !column->is_unused()) {
        AiColumnDefinition *definition = OB_NEWx(AiColumnDefinition, &allocator);
        if (definition == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
        else {
          owned.emplace_back(definition, [](AiColumnDefinition *value) { value->~AiColumnDefinition(); });
          OZ(definition->parse(allocator, column->get_ai_column_definition()));
          if (OB_SUCC(ret)) {
            definitions.push_back(definition);
            columns.push_back(column);
            signature += std::to_string(column->get_column_id()) + ":" +
                std::to_string(definition->definition_version_) + ";";
          }
        }
      }
    }
    if (OB_SUCC(ret) && !definitions.empty()) {
      ColumnBackfill &progress = backfills[table->get_table_id()];
      if (progress.signature != signature) { progress = ColumnBackfill(); progress.signature = signature; }
      if (!progress.done) {
        ret = column_backfill_table(guard, *table, columns, definitions, progress);
        break; // Keep each coordinator round bounded so terminal groups are also published.
      }
    }
  }
  return ret;
}

int AiColumnService::Impl::reconcile()
{
  int ret = OB_SUCCESS;
  if (!reclaimed) {
    ObSqlString sql;
    OZ(sql.append("UPDATE oceanbase.__all_ai_column_task SET state=0,eligible_ts=NOW(6),lease_owner=NULL,lease_expire_ts=NULL WHERE state=1 AND lease_owner NOT LIKE "));
    const std::string prefix = boot + ":%";
    OZ(column_text(sql, column_view(prefix)));
    int64_t affected = 0;
    OZ(GCTX.sql_proxy_->write(sql.ptr(), affected));
    reclaimed = OB_SUCC(ret);
  }
  const int fill = backfill();
  if (fill != OB_SUCCESS && REACH_TIME_INTERVAL(10 * 1000000L)) { LOG_WARN("AI column backfill will retry", K(fill)); }
  std::vector<ColumnTask> terminal;
  {
    ObMySQLProxy::MySQLResult result;
    OZ(GCTX.sql_proxy_->read(result, "SELECT t.table_id,t.rowkey_hash,t.column_id,t.request_id,t.definition_version,t.rowkey_predicate,t.retry_count,t.state FROM oceanbase.__all_ai_column_task t WHERE t.state IN(4,5) AND NOT EXISTS(SELECT 1 FROM oceanbase.__all_ai_column_task p WHERE p.table_id=t.table_id AND p.rowkey_hash=t.rowkey_hash AND p.request_id=t.request_id AND p.state NOT IN(4,5)) ORDER BY t.table_id,t.rowkey_hash,t.request_id LIMIT 64"));
    if (OB_SUCC(ret) && result.get_result() == nullptr) { ret = OB_ERR_UNEXPECTED; }
    while (OB_SUCC(ret)) {
      ret = result.get_result()->next();
      if (ret == OB_ITER_END) { ret = OB_SUCCESS; break; }
      ColumnTask task;
      if (OB_SUCC(ret)) { ret = column_task_fields(*result.get_result(), task); }
      if (OB_SUCC(ret)) { terminal.push_back(std::move(task)); }
    }
  }
  for (size_t i = 0; OB_SUCC(ret) && i < terminal.size(); ++i) { ret = publish(terminal[i]); }
  return ret;
}

AiColumnService::AiColumnService() : lib::ThreadPool(17), impl_(new Impl()) {}
AiColumnService::~AiColumnService() { stop(); wait(); }
int AiColumnService::start()
{
  char uuid[sql::UuidCommon::LENGTH_UUID];
  int ret = sql::ObExprUuid::gen_server_uuid(uuid, sizeof(uuid));
  if (OB_SUCC(ret)) {
    impl_->boot.assign(uuid, sizeof(uuid));
    ret = lib::ThreadPool::start();
  }
  return ret;
}
void AiColumnService::run1()
{
  const uint64_t index = get_thread_idx();
  const std::string owner = impl_->boot + ":" + std::to_string(index);
  int64_t idle = 100000;
  while (!has_set_stop()) {
    THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + 60 * 1000000L);
    int ret = OB_SUCCESS;
    if (GCTX.sql_proxy_ == nullptr || GCTX.schema_service_ == nullptr) { ret = OB_NOT_INIT; }
    else if (index == 0) { ret = impl_->reconcile(); }
    else {
      ColumnTask task;
      bool found = false;
      ret = impl_->claim(owner, task, found);
      if (OB_SUCC(ret) && found) { ret = impl_->process(task); }
      if (OB_SUCC(ret) && found) { idle = 100000; continue; }
    }
    if (ret != OB_SUCCESS && ret != OB_EAGAIN && REACH_TIME_INTERVAL(10 * 1000000L)) { LOG_WARN("AI column worker will retry persistent tasks", K(ret)); }
    ob_usleep(index == 0 ? 200000 : idle);
    idle = std::min<int64_t>(1000000, idle * 2);
  }
}
}
}
