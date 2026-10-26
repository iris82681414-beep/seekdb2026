/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
// Stored with the result column, so definition changes share the schema transaction.
struct AiColumnDefinition
{
  AiColumnDefinition() : max_retries_(3), dimension_(0), state_column_id_(0),
      request_column_id_(0), result_spec_column_id_(0), definition_version_(0), input_is_json_(false) {}
  int parse(common::ObIAllocator &allocator, const common::ObString &text);
  int serialize(common::ObIAllocator &allocator, common::ObString &text) const;
  common::ObString expression_;
  common::ObString normalized_expr_;
  common::ObString model_;
  common::ObString input_expr_;
  common::ObString config_expr_;
  common::ObString system_prompt_;
  int64_t max_retries_;
  int64_t dimension_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  uint64_t definition_version_;
  bool input_is_json_;
  common::ObSEArray<uint64_t, 4> dependencies_;
};
}
}
}
