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

// A bounded exact set for nearby IDs within one document-ID scope. Unsupported
// inputs invalidate the whole result; callers must use their general algorithm.
class DenseIdCounter final
{
public:
  static constexpr uint64_t BIT_COUNT = 1ULL << 18;
  static constexpr uint64_t WORD_COUNT = BIT_COUNT / 64;

  explicit DenseIdCounter(uint64_t *words)
    : words_(words), scope_(0), base_(0), count_(0), initialized_(false),
      supported_(nullptr != words)
  {
    if (supported_) {
      for (uint64_t i = 0; i < WORD_COUNT; ++i) {
        words_[i] = 0;
      }
    }
  }

  bool add(const uint64_t scope, const uint64_t id)
  {
    if (supported_ && !initialized_) {
      scope_ = scope;
      base_ = id & ~(BIT_COUNT - 1);
      initialized_ = true;
    }
    if (!supported_ || scope != scope_ || id < base_ || id - base_ >= BIT_COUNT) {
      supported_ = false;
    } else {
      const uint64_t offset = id - base_;
      const uint64_t mask = 1ULL << (offset & 63);
      uint64_t &word = words_[offset >> 6];
      count_ += (word & mask) == 0;
      word |= mask;
    }
    return supported_;
  }

  bool get_count(int64_t &count) const
  {
    if (supported_) {
      count = count_;
    }
    return supported_;
  }

private:
  uint64_t *words_;
  uint64_t scope_;
  uint64_t base_;
  int64_t count_;
  bool initialized_;
  bool supported_;
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
