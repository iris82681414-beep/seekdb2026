# 全文索引优化：普通 DaaT 批量合并

日期：2026-11-09。

## 基线与验证状态

- 用户提供的平台记录：`c251fef9334dd42f7ebf2c7aa19692bec46bab53` 得分 **7.415 / 50**。
- 恢复提交 `1ee0bb68ab39ce2fe89efd87893aea4099105a80` 与该版本的完整 Git tree 相同。本次在恢复版本上增量修改，保留全文索引构建的编码排序优化。
- 本轮按要求**未构建、未运行单元测试、未运行 SQL 回归或性能基准**。只进行了源码调用链检查、`git diff --check` 与官方基准文件核对。
- 以下测试是待执行的回归用例，不是通过记录。本候选没有平台得分或九项耗时数据，不能宣称达到 50 分、运行正确性已验证或存在已实测的加速。

## 选择实际查询路径

官方四条 MATCH 查询均不按相关度排序。`ObLogPlan::try_push_topn_into_text_retrieval_scan` 只在首个排序项为 MATCH 表达式时设置全文 top-k；普通 `LIMIT 20` 不满足该条件。`ObDASIterUtils::create_text_retrieval_sub_tree` 根据此标记选择 BMW，否则少量查询词进入普通 DaaT。

因此未提交此前仅修改 BMW 所用 `advance_to()` 批内搜索的候选。本次修改实际工厂 `ObDASLegacyTRMergeIter` 创建的 `ObTextDaaTIter`，而不是旧的 SQL 层检索实现。

快路径只在以下条件同时满足时启用：

- 自然语言模式，非函数回查；
- 无相关度计算、投影或过滤，也无 top-k；
- 向量化执行；
- 1～3 个查询词，与现有 small merger 的适用规模相同。

更多查询词、布尔模式、相关度查询和标量执行仍使用原合并实现。实际查询是否满足全部条件、各项收益多少，需要运行时计划与平台耗时验证；不能仅凭 SQL 形态断言全部指标都命中快路径。

## 源码改动

1. `ObTextRetrievalDaaTTokenIter::get_next_doc_id_batch` 通过原有读取和保存流程取得批次，返回迭代器自己持有的 `ObDocIdExt` 数组。保留必要的深拷贝，不借用其他词扫描会复用的表达式缓冲区。
2. `SmallBatchUnion` 在这些有序批次间选择最小文档 ID，一次推进所有相等的批头。跨批次时才请求下一个批次，避免每条 posting 的三次迭代器调用及反复入堆、出堆。每轮至多比较两个批头；这只是算法操作数，不是耗时测量。
3. 直接复用原 `ObSRMergeCmp` 的 datum 比较器。相等键仍选择原堆顺序中的最后一个来源，保留比较相等但字节不同的代表值；单个来源内部的重复项仍按原逻辑分轮处理。
4. 保留 collector 的来源顺序、`minimum_should_match` 判定以及原 `cache_result`/`project_results` 的 LIMIT、OFFSET、结果缓冲与投影。无相关度路径的 collector 输入仍为零。
5. 下一轮才允许覆盖已消费来源的缓冲；在此之前，当前输出已由原结果缓存拷贝。重扫/切换 tablet 时清空批次指针和结束状态。

没有修改分词规则、词典、评分公式、数据量、索引格式、事务可见性或查询结果定义，也没有缓存或硬编码测试答案。

## 待运行的回归

Bazel 目标：`//unittest/storage:test_fts_batch_union`。

六组用例包括：

- 三个来源上四个键的全部子集组合，与生产 `ObSimpleRowsMerger` 逐行参考实现对照完整输出和来源掩码；
- 0～3 个来源、空来源、来源内重复项、不同批次边界、最终非空 `OB_ITER_END`；
- 原 datum 比较器下的 NULL、空串、大小写、尾空格、内嵌 NUL、非 ASCII 字节和 40 字节文档 ID；
- 比较相等但表示不同的键，检查精确代表值；
- collector 的最少匹配词数、批次覆写、耗尽后的重复读取和复用；
- 读取/比较错误，以及非法来源数和非法批次。

这些是合并算法及 collector 的单元级覆盖，不能代替真实 token iterator、SQL 计划、LIMIT/OFFSET、分区重扫及完整查询结果的端到端验证。后续运行验证还需覆盖多批次结果、边界 LIMIT/OFFSET、最少匹配词数、布尔/相关度回退，并与基线逐项比较文档 ID、相关度和规定的排序；不能只比较 COUNT。

## 官方基准冻结

固定配置不变：`ROWS=20000 BATCH=500 ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`。

以下五文件与有分版本逐字节一致，SHA-256 为：

```text
596ea5d1c0038e666b78aa94bef22d2dfb87ffc9e84dbc7e2a1da1f746424462  fts_large_bench.sh
f4e97eddb2e49c982b967fc8047f7cf37177eebe53d648f6746d85aacda91f0c  fts_large_bench_baseline.json
b0c816dd8211ac0471f60b72a37a3fb5c62580357a5b6d7ac95f5a8c2e3f143d  fts_large_bench_gen.py
bea52eb1c68d05693bb3ac5b910ab0e2fa0e70e01582f40466898ee1e2e3ef55  fts_large_bench_score.py
311826751df7f51ac45156fb4284aeca9bd9197370aceb88f4fb92832b458108  fts_large_bench_setup.sql
```

后续得分必须使用平台固定基线的九项指标：构建 3 项、TOKENIZE 2 项、查询 4 项，分别求平均改善率，再对三类等权平均，保留负退化。任务满分要求该最终平均改善率至少 50%。候选相对上一个修改版本的局部加速不能替代这一计算，执行成功也不代表得分大于零。
