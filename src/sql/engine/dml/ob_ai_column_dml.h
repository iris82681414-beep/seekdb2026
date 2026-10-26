/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
namespace oceanbase
{
namespace sql
{
struct ObDMLModifyRowNode;
class AiColumnDml
{
public:
  static int after_row(ObDMLModifyRowNode &row);
};
}
}
