/*
 * Copyright (c) 2026 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::storage;

namespace {
using WordCounts = std::vector<std::pair<std::string, int64_t>>;

ObObjMeta word_meta(ObCollationType collation)
{
  ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(collation);
  return meta;
}

WordCounts word_counts(const ObFTWordMap &words)
{
  WordCounts result;
  for (auto it = words.begin(); it != words.end(); ++it) {
    const ObString text = it->first.get_word().get_string();
    result.emplace_back(std::string(text.ptr(), text.length()), it->second);
  }
  std::sort(result.begin(), result.end());
  return result;
}

// Keep the old two-lookup algorithm as a semantic reference, not a benchmark.
int reference_group_word(ObFTWordMap &words, const ObFTWord &word, int64_t frequency)
{
  int64_t count = 0;
  const int ret = words.get_refactored(word, count);
  if (ret != OB_SUCCESS && ret != OB_HASH_NOT_EXIST) {
    return ret;
  }
  count = ret == OB_HASH_NOT_EXIST ? 1 : count + frequency;
  return words.set_refactored(word, count, 1 /* overwrite */);
}

TEST(FTSWordAggregation, matches_reference_and_preserves_latest_collation_equal_key)
{
  for (const auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN}) {
    ObArenaAllocator allocator;
    ObFTWordMap words;
    ObFTWordMap reference;
    ASSERT_EQ(OB_SUCCESS, words.create(2, "FTSTest"));
    ASSERT_EQ(OB_SUCCESS, reference.create(2, "FTSTest"));
    const ObObjMeta meta = word_meta(collation);
    ObFTParserProperty property;
    ObAddWordFlag flag;
    flag.set_groupby_word();
    ObAddWord add_word(property, meta, flag, allocator, words);
    const std::vector<std::string> input = {"OceanBase", "oceanbase", "OCEANBASE", "résumé", "resume", "database"};
    const int64_t frequencies[] = {4, 2, 7, 3, 5, 1};
    int64_t document_length = 0;
    for (size_t i = 0; i < input.size(); ++i) {
      const auto &text = input[i];
      const ObFTWord word(text.size(), text.data(), meta);
      ASSERT_EQ(OB_SUCCESS, reference_group_word(reference, word, frequencies[i]));
      ASSERT_EQ(OB_SUCCESS, add_word.process_word(text.data(), text.size(), text.size(), frequencies[i]));
      document_length += frequencies[i];
      EXPECT_EQ(document_length, add_word.get_add_word_count());
      EXPECT_EQ(word_counts(reference), word_counts(words));
      if (i == 0) {
        ASSERT_EQ(1, words.size());
        EXPECT_EQ(1, words.begin()->second);
      }
    }
    if (collation == CS_TYPE_UTF8MB4_GENERAL_CI) {
      int64_t count = 0;
      ASSERT_EQ(OB_SUCCESS, words.get_refactored(ObFTWord(9, "OceanBase", meta), count));
      EXPECT_EQ(10, count);
      for (auto it = words.begin(); it != words.end(); ++it) {
        if (it->first == ObFTWord(9, "OceanBase", meta)) {
          EXPECT_EQ(input[2].data(), it->first.get_word().ptr_);
        }
      }
    }
    EXPECT_EQ(OB_SUCCESS, reference.destroy());
    EXPECT_EQ(OB_SUCCESS, words.destroy());
  }
}

TEST(FTSWordAggregation, non_grouped_duplicates_and_invalid_tokens_do_not_advance_count)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(2, "FTSTest"));
  ObFTParserProperty property;
  ObAddWordFlag flag;
  const ObObjMeta meta = word_meta(CS_TYPE_UTF8MB4_BIN);
  ObAddWord add_word(property, meta, flag, allocator, words);
  EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word(nullptr, 3, 3, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word("", 0, 1, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word("word", 4, 0, 1));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word("word", 4, 4, 0));
  EXPECT_EQ(OB_INVALID_ARGUMENT, add_word.process_word("word", 4, 4, -1));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("word", 4, 4, 5));
  EXPECT_EQ(5, add_word.get_add_word_count());
  EXPECT_EQ(OB_HASH_EXIST, add_word.process_word("word", 4, 4, 2));
  EXPECT_EQ(5, add_word.get_add_word_count());
  EXPECT_EQ(1, words.begin()->second);
  EXPECT_EQ(OB_SUCCESS, words.destroy());

  ObFTWordMap uninitialized;
  flag.set_groupby_word();
  ObAddWord uninitialized_add(property, meta, flag, allocator, uninitialized);
  EXPECT_EQ(OB_NOT_INIT, uninitialized_add.process_word("word", 4, 4, 1));
  EXPECT_EQ(0, uninitialized_add.get_add_word_count());
}

TEST(FTSWordAggregation, case_stopword_and_length_filters_preserve_counts)
{
  ObArenaAllocator allocator;
  ObFTWordMap words;
  ASSERT_EQ(OB_SUCCESS, words.create(2, "FTSTest"));
  ObFTParserProperty property;
  property.min_token_size_ = 3;
  property.max_token_size_ = 6;
  ObAddWordFlag flag;
  flag.set_groupby_word();
  flag.set_casedown();
  flag.set_stop_word();
  flag.set_min_max_word();
  ObAddWord add_word(property, word_meta(CS_TYPE_UTF8MB4_BIN), flag, allocator, words);
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("xy", 2, 2, 9));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("toolong", 7, 7, 8));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("THE", 3, 3, 7));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("CAT", 3, 3, 3));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("cat", 3, 3, 2));
  ASSERT_EQ(OB_SUCCESS, add_word.process_word("ABCDEF", 6, 6, 4));
  EXPECT_EQ(9, add_word.get_add_word_count());
  EXPECT_EQ((WordCounts{{"abcdef", 1}, {"cat", 3}}), word_counts(words));
  EXPECT_EQ(OB_SUCCESS, words.destroy());
}

TEST(BENGTokenPipeline, matches_scratch_normalization_and_owns_retained_tokens)
{
  for (const auto collation : {CS_TYPE_UTF8MB4_GENERAL_CI, CS_TYPE_UTF8MB4_BIN, CS_TYPE_LATIN1_SWEDISH_CI}) {
    for (const std::string source : {std::string("The (QUICK), brown fox; database DATABASE! 123 ..."),
                                   std::string("École résumé OceanBase 稳定 DATABASE"),
                                   std::string("... !!!"), std::string("  a ab abc "),
                                   std::string("DATABASE ") + std::string(100000, 'X'),
                                   std::string("valid \xfftoken", 12)}) {
      SCOPED_TRACE(static_cast<int>(collation));
      ObArenaAllocator output_allocator;
      ObArenaAllocator pipeline_allocator;
      ObArenaAllocator reference_allocator;
      const ObCharsetInfo *cs = ObCharset::get_charset(collation);
      ASSERT_NE(nullptr, cs);
      ObTextAnalysisCtx ctx;
      ctx.cs_ = cs;
      ctx.filter_stopword_ = false;
      ctx.need_grouping_ = false;
      ObEnglishTextAnalyzer reference;
      ASSERT_EQ(OB_SUCCESS, reference.init(ctx, reference_allocator));
      ObDatum document;
      document.set_string(source.data(), source.size());
      ObITokenStream *reference_stream = nullptr;
      ASSERT_EQ(OB_SUCCESS, reference.analyze(document, reference_stream));
      std::string mutable_source = source;
      std::vector<ObString> retained;
      std::vector<std::string> expected;
      ObFTWordMap words;
      ASSERT_EQ(OB_SUCCESS, words.create(2, "FTSTest"));
      const ObObjMeta meta = word_meta(collation);
      ObFTParserProperty property;
      ObAddWordFlag flag;
      flag.set_groupby_word();
      ObAddWord add_word(property, meta, flag, output_allocator, words);
      {
        // Deliberately separate object/pipeline storage from emitted word storage.
        ObBEngFTParser parser(output_allocator);
        ObFTParserParam param;
        param.cs_ = cs;
        param.fulltext_ = mutable_source.data();
        param.ft_length_ = mutable_source.size();
        param.allocator_ = &pipeline_allocator;
        ASSERT_EQ(OB_SUCCESS, parser.init(&param));
        while (true) {
          ObDatum reference_token;
          int64_t reference_frequency = 0;
          int reference_ret = reference_stream->get_next(reference_token, reference_frequency);
          if (reference_ret == OB_SUCCESS &&
              (reference_token.ptr_ == nullptr || reference_token.len_ == 0 || reference_frequency <= 0)) {
            reference_ret = OB_INVALID_ARGUMENT;
          }
          const char *word = nullptr;
          int64_t length = 0;
          int64_t char_length = 0;
          int64_t frequency = 0;
          const int ret = parser.get_next_token(word, length, char_length, frequency);
          ASSERT_EQ(reference_ret, ret);
          if (ret != OB_SUCCESS) {
            EXPECT_EQ(nullptr, word);
            EXPECT_EQ(0, length);
            EXPECT_EQ(0, char_length);
            EXPECT_EQ(0, frequency);
            break;
          }
          ASSERT_NE(nullptr, word);
          EXPECT_EQ(reference_token.len_, length);
          EXPECT_EQ(length, char_length);
          EXPECT_EQ(reference_frequency, frequency);
          expected.emplace_back(reference_token.ptr_, reference_token.len_);
          EXPECT_EQ(expected.back(), std::string(word, length));
          retained.emplace_back(length, word);
          ASSERT_EQ(OB_SUCCESS, add_word.process_word(word, length, char_length, frequency));
          for (size_t i = 0; i < retained.size(); ++i) {
            EXPECT_EQ(expected[i], std::string(retained[i].ptr(), retained[i].length()));
          }
        }
      }
      pipeline_allocator.reset();
      std::fill(mutable_source.begin(), mutable_source.end(), '#');
      for (size_t i = 0; i < retained.size(); ++i) {
        EXPECT_EQ(expected[i], std::string(retained[i].ptr(), retained[i].length()));
      }
      // Compare complete collation-aware word maps after parser destruction.
      ObFTWordMap reference_words;
      ASSERT_EQ(OB_SUCCESS, reference_words.create(2, "FTSTest"));
      for (const auto &text : expected) {
        ASSERT_EQ(OB_SUCCESS, reference_group_word(reference_words, ObFTWord(text.size(), text.data(), meta), 1));
      }
      EXPECT_EQ(word_counts(reference_words), word_counts(words));
      EXPECT_EQ(static_cast<int64_t>(expected.size()), add_word.get_add_word_count());
      EXPECT_EQ(OB_SUCCESS, reference_words.destroy());
      EXPECT_EQ(OB_SUCCESS, words.destroy());
    }
  }
}

TEST(BENGTokenPipeline, durable_analyzer_reuse_and_default_reinitialization)
{
  ObArenaAllocator pipeline_allocator;
  ObArenaAllocator output_allocator;
  ObTextAnalysisCtx ctx;
  ctx.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  ctx.filter_stopword_ = false;
  ObEnglishTextAnalyzer analyzer;
  ObDatum document;
  document.set_string("FIRST SECOND", 12);
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, pipeline_allocator, output_allocator));
  ObITokenStream *stream = nullptr;
  ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
  ObDatum token;
  int64_t frequency = 0;
  ASSERT_EQ(OB_SUCCESS, stream->get_next(token, frequency));
  const ObString first = token.get_string();
  ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
  int ret = OB_SUCCESS;
  while ((ret = stream->get_next(token, frequency)) == OB_SUCCESS) {}
  EXPECT_EQ(OB_ITER_END, ret);
  EXPECT_EQ("first", std::string(first.ptr(), first.length()));
  analyzer.reset();
  EXPECT_EQ("first", std::string(first.ptr(), first.length()));
  const int64_t output_used = output_allocator.used();
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, pipeline_allocator));
  ASSERT_EQ(OB_SUCCESS, analyzer.analyze(document, stream));
  ASSERT_EQ(OB_SUCCESS, stream->get_next(token, frequency));
  EXPECT_EQ("first", std::string(token.ptr_, token.len_));
  EXPECT_EQ(output_used, output_allocator.used());
  analyzer.reset();

  ObTextAnalysisCtx invalid_ctx;
  EXPECT_EQ(OB_INVALID_ARGUMENT, analyzer.init(invalid_ctx, pipeline_allocator, output_allocator));
  ASSERT_EQ(OB_SUCCESS, analyzer.init(ctx, pipeline_allocator));
  EXPECT_EQ(OB_INIT_TWICE, analyzer.init(ctx, pipeline_allocator, output_allocator));
  EXPECT_EQ(OB_NOT_INIT, analyzer.analyze(document, stream));
}

TEST(BENGTokenPipeline, default_and_durable_analyzers_keep_filter_and_grouping_behavior)
{
  for (const bool filter : {false, true}) {
    for (const bool group : {false, true}) {
      ObArenaAllocator scratch_allocator;
      ObArenaAllocator pipeline_allocator;
      ObArenaAllocator output_allocator;
      ObTextAnalysisCtx ctx;
      ctx.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
      ctx.filter_stopword_ = filter;
      ctx.need_grouping_ = group;
      ObEnglishTextAnalyzer scratch;
      ObEnglishTextAnalyzer durable;
      ASSERT_EQ(OB_SUCCESS, scratch.init(ctx, scratch_allocator));
      ASSERT_EQ(OB_SUCCESS, durable.init(ctx, pipeline_allocator, output_allocator));
      ObDatum document;
      document.set_string("CAT cat ... DOG dog", 19);
      ObITokenStream *scratch_stream = nullptr;
      ObITokenStream *durable_stream = nullptr;
      ASSERT_EQ(OB_SUCCESS, scratch.analyze(document, scratch_stream));
      ASSERT_EQ(OB_SUCCESS, durable.analyze(document, durable_stream));
      WordCounts scratch_tokens;
      WordCounts durable_tokens;
      for (const bool use_durable : {false, true}) {
        ObITokenStream *stream = use_durable ? durable_stream : scratch_stream;
        WordCounts &tokens = use_durable ? durable_tokens : scratch_tokens;
        ObDatum token;
        int64_t frequency = 0;
        int ret = OB_SUCCESS;
        while ((ret = stream->get_next(token, frequency)) == OB_SUCCESS) {
          tokens.emplace_back(std::string(token.ptr_, token.len_), frequency);
        }
        EXPECT_EQ(OB_ITER_END, ret);
        std::sort(tokens.begin(), tokens.end());
      }
      EXPECT_EQ(scratch_tokens, durable_tokens);
      if (group) {
        EXPECT_EQ((WordCounts{{"cat", 2}, {"dog", 2}}), durable_tokens);
      }
    }
  }
}

TEST(BENGTokenPipeline, invalid_parser_input_and_eof_outputs)
{
  ObArenaAllocator allocator;
  ObBEngFTParser parser(allocator);
  const char *word = "sentinel";
  int64_t length = 1;
  int64_t char_length = 1;
  int64_t frequency = 1;
  EXPECT_EQ(OB_NOT_INIT, parser.get_next_token(word, length, char_length, frequency));
  EXPECT_EQ(nullptr, word);
  EXPECT_EQ(0, length);
  EXPECT_EQ(0, char_length);
  EXPECT_EQ(0, frequency);
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(nullptr));
  ObFTParserParam param;
  param.cs_ = ObCharset::get_charset(CS_TYPE_UTF8MB4_BIN);
  param.allocator_ = &allocator;
  param.fulltext_ = "";
  EXPECT_EQ(OB_INVALID_ARGUMENT, parser.init(&param));
  param.fulltext_ = "...";
  param.ft_length_ = 3;
  ASSERT_EQ(OB_SUCCESS, parser.init(&param));
  EXPECT_EQ(OB_ITER_END, parser.get_next_token(word, length, char_length, frequency));
  EXPECT_EQ(nullptr, word);
  EXPECT_EQ(0, length);
  EXPECT_EQ(0, char_length);
  EXPECT_EQ(0, frequency);
}
} // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  if (ObCharset::init_charset() != OB_SUCCESS || ObFTParseData::init_global() != OB_SUCCESS) {
    ObFTParseData::deinit_global();
    return 1;
  }
  const int result = RUN_ALL_TESTS();
  ObFTParseData::deinit_global();
  return result;
}
