/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#define USING_LOG_PREFIX SHARE_SCHEMA
#include "share/schema/ob_ai_column_schema.h"
#include "lib/ob_check_macros.h"
#include "common/json_type/ob_json_tree.h"
#include "common/json_type/ob_json_bin.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
using namespace common;

int AiColumnDefinition::serialize(ObIAllocator &allocator, ObString &text) const
{
  int ret = OB_SUCCESS;
  ObJsonObject object(&allocator);
  auto add_text = [&](const char *key, const ObString &value) -> int {
    ObJsonString *node = OB_NEWx(ObJsonString, &allocator, value);
    return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : object.add(key, node);
  };
  auto add_number = [&](const char *key, uint64_t value) -> int {
    ObJsonUint *node = OB_NEWx(ObJsonUint, &allocator, value);
    return node == nullptr ? OB_ALLOCATE_MEMORY_FAILED : object.add(key, node);
  };
  OZ(add_number("version", 1));
  OZ(add_text("expression", expression_));
  OZ(add_text("normalized_expr", normalized_expr_));
  OZ(add_text("model", model_));
  OZ(add_text("input_expr", input_expr_));
  OZ(add_text("config_expr", config_expr_));
  OZ(add_text("system_prompt", system_prompt_));
  OZ(add_number("max_retries", max_retries_));
  OZ(add_number("dimension", dimension_));
  OZ(add_number("state_column_id", state_column_id_));
  OZ(add_number("request_column_id", request_column_id_));
  OZ(add_number("result_spec_column_id", result_spec_column_id_));
  OZ(add_number("definition_version", definition_version_));
  OZ(add_number("input_is_json", input_is_json_ ? 1 : 0));
  ObJsonArray array(&allocator);
  for (int64_t i = 0; OB_SUCC(ret) && i < dependencies_.count(); ++i) {
    ObJsonUint *node = OB_NEWx(ObJsonUint, &allocator, dependencies_.at(i));
    if (node == nullptr) { ret = OB_ALLOCATE_MEMORY_FAILED; }
    else { ret = array.append(node); }
  }
  OZ(object.add("dependencies", &array));
  ObJsonBuffer buffer(&allocator);
  OZ(object.print(buffer, false));
  if (OB_SUCC(ret) && buffer.length() > 65535) { ret = OB_SIZE_OVERFLOW; }
  OZ(ob_write_string(allocator, buffer.string(), text));
  return ret;
}

int AiColumnDefinition::parse(ObIAllocator &allocator, const ObString &text)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;
  if (text.empty() || text.length() > 65535) { return OB_INVALID_ARGUMENT; }
  OZ(ObJsonBaseFactory::get_json_base(&allocator, text,
      ObJsonInType::JSON_TREE, ObJsonInType::JSON_TREE, base));
  if (OB_SUCC(ret) && (base == nullptr || base->json_type() != ObJsonNodeType::J_OBJECT)) {
    ret = OB_INVALID_ARGUMENT;
  }
  if (OB_SUCC(ret)) {
    ObJsonObject &object = *static_cast<ObJsonObject *>(base);
    auto get_text = [&](const char *key, ObString &value) -> int {
      ObJsonNode *node = object.get_value(key);
      if (node == nullptr || node->json_type() != ObJsonNodeType::J_STRING) { return OB_INVALID_ARGUMENT; }
      value = static_cast<ObJsonString *>(node)->get_str();
      return OB_SUCCESS;
    };
    auto get_number = [&](const char *key, uint64_t &value) -> int {
      ObJsonNode *node = object.get_value(key);
      if (node == nullptr) { return OB_INVALID_ARGUMENT; }
      if (node->json_type() == ObJsonNodeType::J_UINT) { value = node->get_uint(); }
      else if (node->json_type() == ObJsonNodeType::J_INT && node->get_int() >= 0) { value = node->get_int(); }
      else { return OB_INVALID_ARGUMENT; }
      return OB_SUCCESS;
    };
    uint64_t version = 0;
    uint64_t retries = 0;
    uint64_t dimension = 0;
    OZ(get_number("version", version));
    if (OB_SUCC(ret) && version != 1) { ret = OB_NOT_SUPPORTED; }
    OZ(get_text("expression", expression_));
    OZ(get_text("normalized_expr", normalized_expr_));
    OZ(get_text("model", model_));
    OZ(get_text("input_expr", input_expr_));
    OZ(get_text("config_expr", config_expr_));
    OZ(get_text("system_prompt", system_prompt_));
    OZ(get_number("max_retries", retries));
    OZ(get_number("dimension", dimension));
    OZ(get_number("state_column_id", state_column_id_));
    OZ(get_number("request_column_id", request_column_id_));
    OZ(get_number("result_spec_column_id", result_spec_column_id_));
    OZ(get_number("definition_version", definition_version_));
    uint64_t input_is_json = 0;
    if (OB_SUCC(ret) && object.get_value("input_is_json") != nullptr) {
      ret = get_number("input_is_json", input_is_json);
    }
    if (OB_SUCC(ret) && input_is_json > 1) { ret = OB_INVALID_ARGUMENT; }
    input_is_json_ = input_is_json == 1;
    if (OB_SUCC(ret) && (retries > 100 || dimension > 65535)) { ret = OB_INVALID_ARGUMENT; }
    max_retries_ = retries;
    dimension_ = dimension;
    dependencies_.reset();
    ObJsonNode *array = object.get_value("dependencies");
    if (OB_SUCC(ret) && (array == nullptr || array->json_type() != ObJsonNodeType::J_ARRAY)) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (uint64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
      ObJsonNode *node = static_cast<ObJsonArray *>(array)->get_value(i);
      if (node == nullptr) { ret = OB_INVALID_ARGUMENT; }
      else if (node->json_type() == ObJsonNodeType::J_UINT) { ret = dependencies_.push_back(node->get_uint()); }
      else if (node->json_type() == ObJsonNodeType::J_INT && node->get_int() >= 0) {
        ret = dependencies_.push_back(node->get_int());
      } else { ret = OB_INVALID_ARGUMENT; }
    }
  }
  return ret;
}
}
}
}
