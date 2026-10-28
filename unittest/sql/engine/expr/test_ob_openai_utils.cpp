// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include <gtest/gtest.h>
#include <string>
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

using namespace oceanbase::common;

namespace {
TEST(OpenAIUtils, restore_embedding_indices)
{
  ObArenaAllocator allocator;
  ObString json("{\"data\":[{\"index\":1,\"embedding\":[3,4]},{\"index\":0,\"embedding\":[1,2]}]}");
  ObJsonObject *response = nullptr;
  ObIJsonBase *result = nullptr;
  ObString printed;
  ObOpenAIUtils::ObOpenAIEmbed provider;
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, response));
  ASSERT_EQ(OB_SUCCESS, provider.parse_output(allocator, response, result));
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::print_json_to_str(allocator, result, printed));
  EXPECT_EQ("[[1, 2], [3, 4]]", std::string(printed.ptr(), printed.length()));
}

TEST(OpenAIUtils, reject_invalid_embeddings)
{
  for (const char *body : {
      "{\"data\":{}}",
      "{\"data\":[{\"index\":0,\"embedding\":[]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[\"1\"]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[null]}]}",
      "{\"data\":[{\"index\":-1,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":0.5,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":2,\"embedding\":[1]}]}",
      "{\"data\":[{\"index\":0,\"embedding\":[1]},{\"index\":0,\"embedding\":[2]}]}"}) {
    ObArenaAllocator allocator;
    ObString json(body);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ObOpenAIUtils::ObOpenAIEmbed provider;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result)) << body;
  }
}

TEST(OpenAIUtils, reject_empty_or_non_string_completions)
{
  for (const char *body : {"{}", "{\"choices\":[]}",
      "{\"choices\":[{\"message\":{\"content\":null}}]}",
      "{\"choices\":[{\"message\":{\"content\":\"\"}}]}"}) {
    ObArenaAllocator allocator;
    ObString json(body);
    ObJsonObject *response = nullptr;
    ObIJsonBase *result = nullptr;
    ObOpenAIUtils::ObOpenAIComplete provider;
    ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, response));
    EXPECT_EQ(OB_INVALID_DATA, provider.parse_output(allocator, response, result));
  }
}

TEST(OllamaUtils, native_request_options_and_optional_key)
{
  ObArenaAllocator allocator;
  ObOllamaUtils::ObOllamaComplete provider;
  ObArray<ObString> headers;
  ObString key;
  ASSERT_EQ(OB_SUCCESS, provider.get_header(allocator, key, headers));
  ASSERT_EQ(1, headers.count());
  ObString json("{\"temperature\":0.2,\"max_tokens\":16}");
  ObJsonObject *config = nullptr;
  ObJsonObject *body = nullptr;
  ObString model("model"), system("system"), prompt("hello");
  ASSERT_EQ(OB_SUCCESS, ObAIFuncJsonUtils::get_json_object_form_str(allocator, json, config));
  ASSERT_EQ(OB_SUCCESS, provider.get_body(allocator, model, system, prompt, config, body));
  ASSERT_NE(nullptr, body->get_value("stream"));
  EXPECT_FALSE(body->get_value("stream")->get_boolean());
  ASSERT_NE(nullptr, body->get_value("options"));
  auto *options = static_cast<ObJsonObject *>(body->get_value("options"));
  ASSERT_NE(nullptr, options->get_value("num_predict"));
  EXPECT_EQ(16, options->get_value("num_predict")->get_int());
  EXPECT_EQ(nullptr, options->get_value("max_tokens"));
}
} // namespace
