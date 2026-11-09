/*
 * Copyright (c) 2025 OceanBase.
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

#ifndef OB_SPARSE_UTILS_H_
#define OB_SPARSE_UTILS_H_

#include "ob_i_sparse_retrieval_iter.h"

namespace oceanbase
{
namespace sql
{
struct ObFtsEvalNode;
}
namespace storage
{

// Merge a few ascending streams without per-row iterator or heap operations.
// Each fetched batch belongs to its source and stays valid until that source is
// fetched again. The returned row must be consumed before the next next() call.
// Equal heads are consumed together, in source order; duplicates within a single
// source remain separate rounds, just as in the row-at-a-time DaaT merger.
template <typename T>
class SmallBatchUnion
{
public:
  static constexpr int64_t MAX_STREAMS = 3;
  SmallBatchUnion() { reuse(); }
  void reuse()
  {
    for (int64_t i = 0; i < MAX_STREAMS; ++i) {
      batches_[i] = nullptr;
      counts_[i] = positions_[i] = 0;
      ended_[i] = false;
    }
  }

  // fetch(source, batch, count) may return OB_ITER_END with a final nonempty
  // batch. compare(left, right, result) retains the caller's ordering semantics.
  // matches has one bit for every source contributing to the returned row.
  template <typename Fetch, typename Compare>
  int next(const int64_t stream_count, Fetch &&fetch, Compare &&compare,
           const T *&row, uint64_t &matches)
  {
    int ret = OB_SUCCESS;
    row = nullptr;
    matches = 0;
    if (OB_UNLIKELY(stream_count < 0 || stream_count > MAX_STREAMS)) {
      ret = OB_INVALID_ARGUMENT;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < stream_count; ++i) {
      if (positions_[i] == counts_[i] && !ended_[i]) {
        const T *batch = nullptr;
        int64_t count = 0;
        const int fetch_ret = fetch(i, batch, count);
        if (OB_SUCCESS != fetch_ret && OB_ITER_END != fetch_ret) {
          ret = fetch_ret;
        } else if (OB_UNLIKELY(count < 0 || (count > 0 && nullptr == batch)
                              || (0 == count && OB_SUCCESS == fetch_ret))) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          batches_[i] = batch;
          counts_[i] = count;
          positions_[i] = 0;
          ended_[i] = OB_ITER_END == fetch_ret;
        }
      }
    }
    const T *candidate = nullptr;
    uint64_t candidate_matches = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < stream_count; ++i) {
      if (positions_[i] < counts_[i]) {
        const T *head = &batches_[i][positions_[i]];
        int cmp_ret = 0;
        if (nullptr == candidate) {
          candidate = head;
          candidate_matches = uint64_t{1} << i;
        } else if (OB_FAIL(compare(*head, *candidate, cmp_ret))) {
        } else if (cmp_ret < 0) {
          candidate = head;
          candidate_matches = uint64_t{1} << i;
        } else if (0 == cmp_ret) {
          // The original heap breaks ties by source index and returns the last
          // equal head, so retain the same representative even for equal keys
          // whose byte representations differ under the comparator.
          candidate = head;
          candidate_matches |= uint64_t{1} << i;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (nullptr == candidate) {
        ret = OB_ITER_END;
      } else {
        row = candidate;
        matches = candidate_matches;
        for (int64_t i = 0; i < stream_count; ++i) {
          if (0 != (matches & (uint64_t{1} << i))) {
            ++positions_[i];
          }
        }
      }
    }
    return ret;
  }
private:
  const T *batches_[MAX_STREAMS];
  int64_t counts_[MAX_STREAMS];
  int64_t positions_[MAX_STREAMS];
  bool ended_[MAX_STREAMS];
};

struct ObSRDaaTRelevanceCollector
{
  ObSRDaaTRelevanceCollector() {}
  virtual ~ObSRDaaTRelevanceCollector() {}

  virtual void reset() = 0;
  virtual void reuse() = 0;
  virtual int collect_one_dim(const int64_t dim_idx, const double relevance) = 0;
  virtual int get_result(double &relevance, bool &is_valid) = 0;
};

struct ObSRDaaTInnerProductRelevanceCollector : ObSRDaaTRelevanceCollector
{
  ObSRDaaTInnerProductRelevanceCollector() : ObSRDaaTRelevanceCollector(),
      total_relevance_(0),
      matched_cnt_(0),
      should_match_(0) {}
  virtual ~ObSRDaaTInnerProductRelevanceCollector() {};

  int init(int64_t should_match = 0);
  virtual void reset() override;
  virtual void reuse() override;
  virtual int collect_one_dim(const int64_t dim_idx, const double) override;
  virtual int get_result(double &relevance, bool &is_valid) override;
private:
  double total_relevance_;
  int64_t matched_cnt_;
  int64_t should_match_;
};

struct ObSRDaaTBooleanRelevanceCollector : ObSRDaaTRelevanceCollector
{
  ObSRDaaTBooleanRelevanceCollector() : ObSRDaaTRelevanceCollector(),
      allocator_(nullptr), dim_cnt_(0), boolean_compute_node_(nullptr), boolean_relevances_() {}
  virtual ~ObSRDaaTBooleanRelevanceCollector() {};

  int init(ObIAllocator *allocator, const int64_t dim_cnt, sql::ObFtsEvalNode *node);
  virtual void reset() override;
  virtual void reuse() override;
  virtual int collect_one_dim(const int64_t dim_idx, const double) override;
  virtual int get_result(double &relevance, bool &is_valid) override;
private:
  ObIAllocator *allocator_;
  int64_t dim_cnt_;
  sql::ObFtsEvalNode *boolean_compute_node_;
  ObFixedArray<double, ObIAllocator> boolean_relevances_;
};

} // namespace storage
} // namespace oceanbase

#endif
