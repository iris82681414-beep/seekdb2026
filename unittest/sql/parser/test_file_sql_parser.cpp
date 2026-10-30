// Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0.
#include <gtest/gtest.h>
#include <string>
#include "sql/parser/ob_parser.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace {
const ParseNode *find_node(const ParseNode *node, ObItemType type)
{
  const ParseNode *found = nullptr;
  if (node != nullptr) {
    if (node->type_ == type) {
      found = node;
    } else {
      for (int i = 0; i < node->num_child_ && found == nullptr; ++i) {
        found = find_node(node->children_[i], type);
      }
    }
  }
  return found;
}

TEST(FileSqlParser, file_functions_keep_arguments_and_aliases)
{
  for (const char *name : {"read_csv", "read_json", "read_ndjson", "read_parquet", "glob"}) {
    for (const char *alias : {"", " f", " AS f"}) {
      SCOPED_TRACE(std::string(name) + alias);
      ObArenaAllocator allocator;
      ObParser parser(allocator, 0);
      ParseResult result = {};
      const std::string query = std::string("SELECT * FROM ") + name + "('/tmp/input')" + alias;
      ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(query.size(), query.data()), result));
      const ParseNode *table = find_node(result.result_tree_, T_FILE_SQL_TABLE_EXPRESSION);
      ASSERT_NE(nullptr, table);
      ASSERT_EQ(3, table->num_child_);
      ASSERT_NE(nullptr, table->children_[0]);
      ASSERT_NE(nullptr, table->children_[1]);
      EXPECT_EQ(std::string(name), std::string(table->children_[0]->str_value_, table->children_[0]->str_len_));
      EXPECT_EQ(alias[0] == '\0', table->children_[2] == nullptr);
      parser.free_result(result);
    }
  }
}

TEST(FileSqlParser, copy_table_and_query_have_native_output_node)
{
  for (const char *query : {
      "COPY my_table TO '/tmp/output.csv' (FORMAT CSV, HEADER)",
      "COPY (SELECT id FROM read_csv('/tmp/input.csv',header=true,sample_size=-1) WHERE id>0) "
      "TO '/tmp/output.csv' (HEADER TRUE, DELIMITER '|', NULLSTR 'NULL')"}) {
    SCOPED_TRACE(query);
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result = {};
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(query), result));
    const ParseNode *copy = find_node(result.result_tree_, T_INTO_FILE_SQL_COPY);
    ASSERT_NE(nullptr, copy);
    ASSERT_EQ(2, copy->num_child_);
    ASSERT_NE(nullptr, copy->children_[0]);
    EXPECT_EQ("/tmp/output.csv", std::string(copy->children_[0]->str_value_, copy->children_[0]->str_len_));
    ASSERT_NE(nullptr, copy->children_[1]);
    parser.free_result(result);
  }
}

TEST(FileSqlParser, describe_and_join_accept_named_file_options)
{
  for (const char *query : {
      "DESCRIBE SELECT * FROM read_csv('/tmp/input.csv',header=true,sample_size=-1)",
      "SELECT c.id FROM read_csv('/tmp/input.csv') c LEFT JOIN "
      "read_json('/tmp/input.jsonl',format='newline_delimited') j ON c.id=j.id "
      "WHERE j.id IS NULL ORDER BY c.id LIMIT 10"}) {
    SCOPED_TRACE(query);
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result = {};
    ASSERT_EQ(OB_SUCCESS, parser.parse(ObString(query), result));
    ASSERT_NE(nullptr, find_node(result.result_tree_, T_FILE_SQL_TABLE_EXPRESSION));
    parser.free_result(result);
  }
}

TEST(FileSqlParser, incomplete_file_sql_is_rejected)
{
  for (const char *query : {"SELECT * FROM read_csv('/tmp/input',header=)",
                           "COPY (SELECT 1) TO", "COPY TO '/tmp/output.csv'"}) {
    SCOPED_TRACE(query);
    ObArenaAllocator allocator;
    ObParser parser(allocator, 0);
    ParseResult result = {};
    EXPECT_NE(OB_SUCCESS, parser.parse(ObString(query), result));
    parser.free_result(result);
  }
}
} // namespace

#ifdef SEEKDB_FILE_SQL_STANDALONE_TEST
int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
