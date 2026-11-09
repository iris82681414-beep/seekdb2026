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
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>
#include "share/datum/ob_datum_funcs.h"
#include "storage/retrieval/ob_sparse_daat_iter.h"

using namespace oceanbase::common;
using namespace oceanbase::storage;

namespace {

// Use the production row-at-a-time heap as the independent reference, including
// its tie order and the rule that each source advances only once per round.
template <typename T, typename Compare>
struct ReferenceCmp
{
  std::array<const T *, 3> &heads_;
  Compare &compare_;
  int cmp(const ObSRMergeItem &left, const ObSRMergeItem &right, int64_t &result)
  {
    int cmp_ret = 0;
    const int ret = compare_(*heads_[left.iter_idx_], *heads_[right.iter_idx_], cmp_ret);
    result = cmp_ret;
    return ret;
  }
};

template <typename T, typename Compare, typename Same>
void check_batches(const std::array<std::vector<T>, 3> &sources,
                   const std::array<int64_t, 3> &widths,
                   const int64_t stream_count, const bool end_with_rows,
                   Compare compare, Same same)
{
  std::vector<std::pair<T, uint64_t>> expected;
  std::array<const T *, 3> heads{};
  std::array<size_t, 3> reference_pos{};
  std::vector<int64_t> pending;
  for (int64_t i = 0; i < stream_count; ++i) {
    pending.push_back(i);
  }
  ReferenceCmp<T, Compare> cmp{heads, compare};
  ObArenaAllocator allocator;
  ObSimpleRowsMerger<ObSRMergeItem, ReferenceCmp<T, Compare>> heap(cmp);
  if (stream_count > 0) {
    ASSERT_EQ(OB_SUCCESS, heap.init(stream_count, stream_count, allocator));
    ASSERT_EQ(OB_SUCCESS, heap.open(stream_count));
  }
  while (!pending.empty()) {
    for (const int64_t i : pending) {
      if (reference_pos[i] < sources[i].size()) {
        heads[i] = &sources[i][reference_pos[i]++];
        ObSRMergeItem item;
        item.iter_idx_ = i;
        ASSERT_EQ(OB_SUCCESS, heap.push(item));
      }
    }
    pending.clear();
    if (heap.empty()) {
      break;
    }
    ASSERT_EQ(OB_SUCCESS, heap.rebuild());
    uint64_t mask = 0;
    const T *last = nullptr;
    bool group_end = false;
    while (!heap.empty() && !group_end) {
      group_end = heap.is_unique_champion();
      const ObSRMergeItem *item = nullptr;
      ASSERT_EQ(OB_SUCCESS, heap.top(item));
      const int64_t idx = item->iter_idx_;
      last = heads[idx];
      mask |= uint64_t{1} << idx;
      pending.push_back(idx);
      ASSERT_EQ(OB_SUCCESS, heap.pop());
    }
    ASSERT_NE(nullptr, last);
    expected.emplace_back(*last, mask);
  }

  SmallBatchUnion<T> merger;
  // Reuse the same merger with freshly rewound sources, as in a rescan/tablet switch.
  for (int pass = 0; pass < 2; ++pass) {
    merger.reuse();
    std::array<std::vector<T>, 3> buffers;
    std::array<size_t, 3> input_pos{};
    std::array<int64_t, 3> fetch_calls{};
    int64_t comparisons = 0;
    for (int64_t i = 0; i < stream_count; ++i) {
      buffers[i].resize(widths[i]);
    }
    auto fetch = [&](const int64_t i, const T *&batch, int64_t &count) {
      ++fetch_calls[i];
      count = std::min(widths[i], static_cast<int64_t>(sources[i].size() - input_pos[i]));
      // Deliberately overwrite a source's previous storage on refill. Other
      // streams' storage stays valid; no expression-evaluation memory is borrowed.
      std::copy_n(sources[i].begin() + input_pos[i], count, buffers[i].begin());
      batch = count > 0 ? buffers[i].data() : nullptr;
      input_pos[i] += count;
      return count == 0 || (end_with_rows && input_pos[i] == sources[i].size())
          ? OB_ITER_END : OB_SUCCESS;
    };
    auto counted_compare = [&](const T &left, const T &right, int &result) {
      ++comparisons;
      return compare(left, right, result);
    };
    for (const auto &entry : expected) {
      const T *row = nullptr;
      uint64_t mask = 0;
      ASSERT_EQ(OB_SUCCESS, merger.next(stream_count, fetch, counted_compare, row, mask));
      ASSERT_NE(nullptr, row);
      ASSERT_TRUE(same(entry.first, *row));
      ASSERT_EQ(entry.second, mask);
    }
    for (int repeat = 0; repeat < 2; ++repeat) {
      const T *row = nullptr;
      uint64_t mask = 7;
      ASSERT_EQ(OB_ITER_END, merger.next(stream_count, fetch, counted_compare, row, mask));
      ASSERT_EQ(nullptr, row);
      ASSERT_EQ(0U, mask);
    }
    for (int64_t i = 0; i < stream_count; ++i) {
      const int64_t batches = (sources[i].size() + widths[i] - 1) / widths[i];
      EXPECT_EQ(batches + ((!end_with_rows || sources[i].empty()) ? 1 : 0), fetch_calls[i]);
    }
    EXPECT_LE(comparisons, 2 * static_cast<int64_t>(expected.size()));
  }
}

int compare_int(const int64_t left, const int64_t right, int &result)
{
  result = left < right ? -1 : (left > right ? 1 : 0);
  return OB_SUCCESS;
}

bool same_int(const int64_t left, const int64_t right) { return left == right; }

TEST(FTSBatchUnion, every_subset_matches_production_heap)
{
  for (int a = 0; a < 16; ++a) {
    for (int b = 0; b < 16; ++b) {
      for (int c = 0; c < 16; ++c) {
        std::array<std::vector<int64_t>, 3> sources;
        const int masks[] = {a, b, c};
        for (int i = 0; i < 3; ++i) {
          for (int bit = 0; bit < 4; ++bit) {
            if (masks[i] & (1 << bit)) {
              sources[i].push_back(bit);
            }
          }
        }
        check_batches(sources, {1, 2, 3}, 3, false, compare_int, same_int);
        check_batches(sources, {3, 1, 2}, 3, true, compare_int, same_int);
      }
    }
  }
}

TEST(FTSBatchUnion, duplicates_empty_streams_and_batch_boundaries)
{
  const int64_t max_id = std::numeric_limits<int64_t>::max();
  const std::array<std::vector<int64_t>, 3> sources = {{
      {-7, -7, 0, 1, 4, 4, 9, 20, max_id},
      {-7, 0, 0, 2, 4, 8, 20},
      {1, 2, 4, 4, 4, 9, max_id}}};
  for (int64_t streams = 0; streams <= 3; ++streams) {
    for (const int64_t width : {1, 2, 4, 16}) {
      for (const bool end_with_rows : {false, true}) {
        check_batches(sources, {width, width, width}, streams, end_with_rows, compare_int, same_int);
      }
    }
  }
  std::array<std::vector<int64_t>, 3> sparse = {{{}, {2, 8, 21}, {}}};
  check_batches(sparse, {1, 2, 3}, 3, true, compare_int, same_int);
}

TEST(FTSBatchUnion, binary_document_ids_and_equal_key_representatives)
{
  std::array<std::vector<ObDocIdExt>, 3> sources;
  std::vector<std::string> words = {
      "", " ", "A", "a", "a ", "aa", "É", "中", "\xf0\x9f\x98\x80",
      std::string("a\0", 2), std::string("\0", 1), "\x80", "\xff", std::string(40, 'x')};
  std::sort(words.begin(), words.end());
  ObDatum datum;
  for (int i = 0; i < 3; ++i) {
    datum.set_null();
    sources[i].emplace_back();
    ASSERT_EQ(OB_SUCCESS, sources[i].back().from_datum(datum));
    for (size_t j = 0; j < words.size(); ++j) {
      if (j % 3 != static_cast<size_t>(i)) {
        datum.set_string(words[j].data(), static_cast<uint32_t>(words[j].size()));
        sources[i].emplace_back();
        ASSERT_EQ(OB_SUCCESS, sources[i].back().from_datum(datum));
      }
    }
  }
  oceanbase::sql::ObDatumMeta meta;
  meta.type_ = ObVarcharType;
  meta.cs_type_ = CS_TYPE_BINARY;
  ObFixedArray<const ObDatum *, ObIAllocator> unused_heads;
  ObSRMergeCmp cmp;
  ASSERT_EQ(OB_SUCCESS, cmp.init(meta, &unused_heads));
  auto compare = [&](const ObDocIdExt &left, const ObDocIdExt &right, int &result) {
    return cmp.cmp(left.get_datum(), right.get_datum(), result);
  };
  auto same = [](const ObDocIdExt &left, const ObDocIdExt &right) {
    const ObDatum &l = left.get_datum();
    const ObDatum &r = right.get_datum();
    return l.is_null() == r.is_null() && (l.is_null()
        || (l.len_ == r.len_ && 0 == std::memcmp(l.ptr_, r.ptr_, l.len_)));
  };
  check_batches(sources, {1, 3, 2}, 3, false, compare, same);
  check_batches(sources, {4, 2, 1}, 3, true, compare, same);

  // Equal-by-comparator but byte-distinct keys must select the same source as
  // the heap, not merely produce an equal comparison result.
  const std::array<std::vector<int64_t>, 3> aliases = {{{10, 20, 30}, {11, 21, 31}, {12, 22, 32}}};
  auto grouped_compare = [](const int64_t left, const int64_t right, int &result) {
    return compare_int(left / 10, right / 10, result);
  };
  check_batches(aliases, {1, 2, 3}, 3, true, grouped_compare, same_int);
}

TEST(FTSBatchUnion, minimum_should_match_uses_all_matching_sources)
{
  const int64_t source[] = {1, 2, 4};
  for (int64_t minimum = 0; minimum <= 4; ++minimum) {
    SmallBatchUnion<int64_t> merger;
    ObSRDaaTInnerProductRelevanceCollector collector;
    ASSERT_EQ(OB_SUCCESS, collector.init(minimum));
    auto fetch = [&](const int64_t i, const int64_t *&batch, int64_t &count) {
      batch = source + i;
      count = 3 - i;
      return OB_ITER_END;
    };
    const uint64_t masks[] = {1, 3, 7};
    for (int64_t row_idx = 0; row_idx < 3; ++row_idx) {
      const int64_t *row = nullptr;
      uint64_t mask = 0;
      ASSERT_EQ(OB_SUCCESS, merger.next(3, fetch, compare_int, row, mask));
      ASSERT_EQ(source[row_idx], *row);
      ASSERT_EQ(masks[row_idx], mask);
      for (int64_t i = 0; i < 3; ++i) {
        if (mask & (uint64_t{1} << i)) {
          ASSERT_EQ(OB_SUCCESS, collector.collect_one_dim(i, 0.0));
        }
      }
      double relevance = -1;
      bool valid = false;
      ASSERT_EQ(OB_SUCCESS, collector.get_result(relevance, valid));
      EXPECT_EQ(0.0, relevance);
      EXPECT_EQ(row_idx + 1 >= minimum, valid);
    }
  }
}

TEST(FTSBatchUnion, errors_do_not_emit_or_consume_heads)
{
  SmallBatchUnion<int64_t> merger;
  const int64_t ids[] = {1, 2, 3};
  bool fail_fetch = true;
  bool fail_compare = true;
  auto fetch = [&](const int64_t i, const int64_t *&batch, int64_t &count) {
    if (i == 1 && fail_fetch) {
      return OB_TIMEOUT;
    }
    batch = ids + i;
    count = 1;
    return OB_ITER_END;
  };
  auto compare = [&](const int64_t left, const int64_t right, int &result) {
    return fail_compare ? OB_ERR_UNEXPECTED : compare_int(left, right, result);
  };
  const int64_t *row = ids;
  uint64_t mask = 7;
  EXPECT_EQ(OB_TIMEOUT, merger.next(3, fetch, compare, row, mask));
  EXPECT_EQ(nullptr, row);
  EXPECT_EQ(0U, mask);
  fail_fetch = false;
  EXPECT_EQ(OB_ERR_UNEXPECTED, merger.next(3, fetch, compare, row, mask));
  EXPECT_EQ(nullptr, row);
  EXPECT_EQ(0U, mask);
  fail_compare = false;
  for (int64_t i = 0; i < 3; ++i) {
    ASSERT_EQ(OB_SUCCESS, merger.next(3, fetch, compare, row, mask));
    EXPECT_EQ(ids[i], *row);
    EXPECT_EQ(uint64_t{1} << i, mask);
  }
}

TEST(FTSBatchUnion, rejects_invalid_batches_and_stream_counts)
{
  SmallBatchUnion<int64_t> merger;
  int calls = 0;
  auto empty_success = [&](const int64_t, const int64_t *&, int64_t &) {
    ++calls;
    return OB_SUCCESS;
  };
  const int64_t *row = nullptr;
  uint64_t mask = 0;
  EXPECT_EQ(OB_INVALID_ARGUMENT, merger.next(-1, empty_success, compare_int, row, mask));
  EXPECT_EQ(OB_INVALID_ARGUMENT, merger.next(4, empty_success, compare_int, row, mask));
  EXPECT_EQ(OB_ITER_END, merger.next(0, empty_success, compare_int, row, mask));
  EXPECT_EQ(0, calls);
  EXPECT_EQ(OB_ERR_UNEXPECTED, merger.next(1, empty_success, compare_int, row, mask));
  for (const int64_t bad_count : {-1, 1}) {
    merger.reuse();
    auto invalid = [&](const int64_t, const int64_t *&batch, int64_t &count) {
      batch = nullptr;
      count = bad_count;
      return OB_ITER_END;
    };
    EXPECT_EQ(OB_ERR_UNEXPECTED, merger.next(1, invalid, compare_int, row, mask));
  }
}

} // namespace

int main(int argc, char **argv)
{
  testing::InitGoogleTest(&argc, argv);
  if (ObCharset::init_charset() != OB_SUCCESS) {
    return 1;
  }
  return RUN_ALL_TESTS();
}
