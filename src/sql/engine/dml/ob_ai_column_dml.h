/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_iarray.h"
namespace oceanbase
{
namespace share { namespace schema { class ObSchemaGetterGuard; } }
namespace sql
{
struct ObDMLModifyRowNode;
class ObDMLStmt;
class ObExecContext;
class AiColumnDml
{
public:
  static int after_row(ObDMLModifyRowNode &row);
  static int collect_reads(const ObDMLStmt &stmt, share::schema::ObSchemaGetterGuard &schema,
      common::ObIArray<uint64_t> &reads);
  static int check_strict_read(ObExecContext &context);
};
}
}
