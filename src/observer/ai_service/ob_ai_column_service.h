/* Copyright (c) 2026 OceanBase. Licensed under the Apache License, Version 2.0. */
#pragma once
#include "lib/thread/thread_pool.h"
#include <memory>

namespace oceanbase
{
namespace observer
{
class AiColumnService final : public lib::ThreadPool
{
public:
  AiColumnService();
  ~AiColumnService() override;
  int start() override;
private:
  void run1() override;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
}
