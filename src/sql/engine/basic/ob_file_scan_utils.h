/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once

#include "common/object/ob_object.h"
#include "common/ob_accuracy.h"
#include "lib/container/ob_iarray.h"

namespace oceanbase
{
namespace sql
{
class ObSQLSessionInfo;
class ObExecContext;

struct FileSqlColumn
{
  common::ObString name_;
  common::ObObjMeta meta_;
  common::ObAccuracy accuracy_;
  bool nullable_ = true;
  uint8_t kind_ = 0;
};

// This owns only a bounded format cursor. SQL expressions, joins and aggregates
// remain ordinary seekdb operators above the function-table leaf.
class FileSqlReader
{
public:
  FileSqlReader();
  ~FileSqlReader();
  static int check_enabled(ObSQLSessionInfo &session);
  int infer(ObSQLSessionInfo &session, ObExecContext *context, common::ObIAllocator &allocator,
            const common::ObString &path, const common::ObString &format,
            const common::ObString &options, common::ObString &definition,
            common::ObIArray<FileSqlColumn> &columns);
  int open(ObSQLSessionInfo &session, const common::ObString &definition,
           const common::ObIArray<uint64_t> &column_ids);
  int next(common::ObIAllocator &allocator, common::ObIArray<common::ObObj> &values);
  int rescan();
  void close();
  static const char *type_name(uint8_t kind);
private:
  struct Impl;
  Impl *impl_;
  DISALLOW_COPY_AND_ASSIGN(FileSqlReader);
};

class FileSqlWriter
{
public:
  FileSqlWriter();
  ~FileSqlWriter();
  int open(ObSQLSessionInfo &session, const common::ObString &path, const common::ObString &definition);
  int field(const common::ObString &text, bool is_null);
  int end_row();
  int finish();
  void close();
private:
  struct Impl;
  Impl *impl_;
  DISALLOW_COPY_AND_ASSIGN(FileSqlWriter);
};

}
}
