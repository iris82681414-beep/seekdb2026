/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_iarray.h"
#include "lib/string/ob_string.h"
namespace oceanbase
{
namespace share { namespace schema { class ObSchemaGetterGuard; } }
namespace sql
{
struct ObDMLModifyRowNode;
struct ObDMLBaseCtDef;
class ObDMLStmt;
class ObExecContext;
class AiColumnDml
{
public:
  static int after_row(ObDMLModifyRowNode &row);
  static int collect_reads(const ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &schema,
      common::ObIArray<uint64_t> &reads);
  static int check_strict_read(ObExecContext &context);
  static bool is_retry_write(const ObDMLBaseCtDef &definition, ObExecContext &context);
  static int retry_columns(ObExecContext &context, uint64_t table_id,
      const common::ObIArray<uint64_t> &columns, const common::ObString &predicate,
      bool check_privilege = true);
};
}
}
