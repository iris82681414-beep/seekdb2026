// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
TEST(AIPrompt, structured_arguments_and_repeated_placeholders)
{
  ObArenaAllocator allocator;
  ObString json("{\"template\":\"{0}/{0}/{1}\",\"args\":[[1,true],{\"k\":2}]}");
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("[1, true]/[1, true]/{\"k\": 2}", std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, long_unicode_and_literal_placeholder_in_argument)
{
  ObArenaAllocator allocator;
  const std::string argument = std::string(50000, 'x') + "世界{1}\\\n";
  ObString text(argument.size(), argument.data());
  ObString format("{0}-{0}");
  ObJsonArray *args = nullptr;
  ObJsonString *value = nullptr;
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_array(allocator, args));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator, text, value));
  ASSERT_EQ(OB_SUCCESS, args->append(value));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::construct_prompt_object(allocator, format, args, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ(argument + "-" + argument, std::string(result.ptr(), result.length()));
}

TEST(AIPrompt, invalid_indices_and_literal_braces)
{
  for (const char *format : {"{1}", "{99999999999999999999999999999999999999999999}"}) {
    ObArenaAllocator allocator;
    ObString json("{\"args\":[\"x\"],\"template\":\"unused\"}");
    ObJsonObject *prompt = nullptr;
    ObJsonString *value = nullptr;
    ObString text(format);
    ObString result;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, prompt));
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_string(allocator, text, value));
    ASSERT_EQ(OB_SUCCESS, prompt->add("template", value));
    EXPECT_EQ(OB_INVALID_ARGUMENT, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  }
  ObArenaAllocator allocator;
  ObString json("{\"args\":[],\"template\":\"{abc} {} {-1} {0\"}");
  ObJsonObject *prompt = nullptr;
  ObString result;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, prompt));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncPromptObjectUtils::replace_all_str_args_in_template(allocator, prompt, result));
  EXPECT_EQ("{abc} {} {-1} {0", std::string(result.ptr(), result.length()));
}
} // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
