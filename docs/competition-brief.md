# 2026 OceanBase 初赛任务说明

截至 2026 年 10 月 26 日，本届初赛共 30 道题、620 分。建议先准备 seekdb 比赛代码和评测环境，完成基础实践，再从可独立计分的 AI 函数子题开始。官方比赛分支已导入本地，实施状态见 [实施进度](competition-progress.md)。

## 时间与排名

- 群公告时间为 2026 年 10 月 26 日 10:00 至 11 月 18 日 24:00，按北京时间记录；截止时刻等同于 11 月 19 日 00:00。
- 排名先比较总分，同分时提交时间更早者靠前。
- 本届允许 AI Coding，每队 1 至 3 人。

来源：[大赛介绍](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/ix20uoszxhifnp4b)、[算分规则](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/xkmkblbzlzk581ya)。具体起止时间来自本次提供的群公告。

## 分值分布

| 方向 | 内容 | 分值 |
| --- | --- | ---: |
| seekdb 基础实践 | 部署、公开 Fork、官方 PR，各 10 分 | 30 |
| seekdb AI 函数 | 6 个子题，各 10 分 | 60 |
| seekdb 文件即数据库 | CSV、JSONL、Parquet 的原生 SQL 查询 | 50 |
| seekdb AI 列 | 异步生成、事务一致性、重试及向量索引维护 | 50 |
| seekdb 全文索引 | 构建、分词、查询性能优化 | 50 |
| PowerContext | 准备仓库 10 分、PDF 题 50 分、其余 9 题各 20 分 | 240 |
| AI 生态 | AMD、MiniMax、Qoder、阶跃星辰、Dify、高德、魔搭，各 20 分 | 140 |
| 合计 | seekdb 240 分，PowerContext 240 分，生态 140 分 | 620 |

来源：[赛题总览](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/ocqe6xv6mlgapq24)、[算分规则](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/xkmkblbzlzk581ya)。

## 仓库与提交

| 用途 | 官方代码 | 指定分支 |
| --- | --- | --- |
| seekdb 相关题目及 AMD | `oceanbase/seekdb` | `2026-competition` |
| PowerContext | `oceanbase/powercontext` | `2026-competition` |
| MiniMax、Qoder、Dify、高德、阶跃星辰 | `oceanbase/powercontext` | `2026-competition-ecosystem` |

生态分支对应目录分别为 `minimax`、`qoder`、`dify`、`gaode`、`stepfun`。魔搭题的提交材料需要按其独立题面准备，上表不推定其目录。

答题仓库须保持私有，并按平台说明配置仓库读取权限。基础实践中的公开 Fork 和官方 PR 是独立材料，可以使用与答题仓库不同的账号或组织。

基础实践的三个得分项由平台独立核验：

1. **部署 10 分。** 启动自己的 seekdb 实例，从运行目录的 `run/telemetry.json` 读取 `content.id`，提交完整 UUID。遥测数据同步约有半小时延迟。
2. **Fork 10 分。** 公开 Fork 官方 `oceanbase/seekdb`，在默认分支 README 中独立一行放入平台生成的本队验证行，再提交 Fork 地址。新建仓库后复制代码不满足 Fork 关系核验。
3. **PR 10 分。** 按 PR 子题要求修复带 `2026-competition` 标签的 Bug，向官方仓库提交 PR，并在 PR 正文中独立一行放入本队验证行。平台说明按提交核验，不要求合并。

三项可以分次提交；已通过的基础实践项目会保留分数，代码评测题则每次重新评测。公开 Fork 的 README 和官方 PR 正文均需各自放入验证行，私有答题仓库不需要添加。

来源：[比赛仓库地址](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/brxxufkegusa86hl)、[GitHub 使用说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qs1povt8zbza6hts)、[基础实践平台说明](https://open.oceanbase.com/train/detail/21?questionId=600071)、[部署题](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lz6ixow11mar233g)、[Bug 修复题](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/xom0q8ni2ow8t9py)。

## seekdb 题目要点

### AI 函数增强

六个子题各 10 分，模型请求由官方 Mock 服务响应，无需真实模型。功能必须实现在 seekdb SQL 引擎内，并保持普通 SQL 和既有 AI 函数行为正确。

| 子题 | 主要工作 |
| --- | --- |
| AI_PROMPT | 补齐 TEXT、LONGTEXT、JSON 及模板占位符处理 |
| AI_EMBED 单行 | 处理维度参数并校验返回向量 |
| AI_COMPLETE | 支持请求配置、长文本及异常响应校验 |
| Ollama Provider | 补齐 Endpoint 管理、免 Key 调用及原生生成和向量协议 |
| AI_RERANK | 处理分批、全局下标、稳定排序和返回结果校验 |
| AI_EMBED 跨行批量 | 合并多行请求，并正确恢复行顺序、NULL 和失败语义 |

官方分支中已确认存在的入口包括：

- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_function_enhancement.test`
- `tools/deploy/mysql_test/test_suite/ai_function/t/ai_prompt.test`

来源：[AI 函数题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/psu9fks8dexldoum)、[比赛分支测试](https://github.com/oceanbase/seekdb/tree/2026-competition/tools/deploy/mysql_test/test_suite/ai_function)。

### 文件即数据库

通过 `read_csv`、`read_json` / `read_ndjson`、`read_parquet` 读取本地文件，由 seekdb 原生执行过滤、聚合、排序和 JOIN。评分分为入口与文件发现 7 分、CSV 10 分、JSONL 7 分、Parquet 6 分、类型推断 6 分、单文件 SQL 6 分、JOIN 4 分、安全和工程质量 4 分。

扫描需要流式执行；功能默认关闭，开启后仍须校验 `secure_file_priv`。不能使用外部 SQL 引擎代执行。题面还要求 CSV COPY 导出及读回。公开测试入口为 `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test`。

题面 COPY 段落残留“总分 30 分”的表述，与题首、详细评分表和平台的 50 分不一致；这里按本届评分表和平台记录为 50 分。

来源：[文件即数据库题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/gy2a66vag0028msk)、[seekdb 代码评测平台](https://open.oceanbase.com/train/detail/21?questionId=600069)。

### AI 列

在数据库内核实现异步 AI 派生列。业务事务提交后才调用模型；依赖列更新需让旧结果失效并重新计算；后台迟到结果不能覆盖新数据。还需实现持久化调度、分组发布、失败重试、严格读取及向量索引维护。基础测试使用 Mock 服务。

评分表对应五组分值为 DDL 和类型约束 8 分、DML 与事务 10 分、调度与写回 12 分、重试与严格读取 10 分、向量和索引 10 分，共 50 分。

官方比赛分支中的测试实际位于 `tools/deploy/mysql_test/test_suite/ai_column/`，与题面中部分写成 `ai_function/` 的路径不同，应以分支实际文件为准。

来源：[AI 列题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qenx4sy5s09h2t07)、[比赛分支 AI 列测试](https://github.com/oceanbase/seekdb/tree/2026-competition/tools/deploy/mysql_test/test_suite/ai_column)。

### 全文索引优化

比赛分值 50 分，内部性能报告使用 100 分制。构建、分词、查询三类耗时提升率分别取平均，再将三类等权平均；综合耗时下降 50% 时内部报告满分。单项退化以负值参与平均，不能把全部九项指标直接等权平均。

必须保持分词和查询结果正确，不能通过修改测试数据、测试 SQL、计时逻辑、评分脚本或基线提高分数。固定基线文件为 `tools/benchmark/fts_large_bench_baseline.json`。本地性能测量应使用专用实例，因为脚本会重建测试数据库。

来源：[全文索引题面](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/za7cfo5cazcrihhq)、[性能题平台](https://open.oceanbase.com/train/detail/21?questionId=600080)。

## 评测耗时与规则冲突

官方时效表列出：seekdb 代码题和性能题各支持 2 个并发，无排队时约 30 分钟；PowerContext 支持 2 个并发，无排队时约 15 分钟。部分生态题需要约一天并可能人工审核，宜提前准备提交材料。

目录中的旧“测试注意事项”使用 `vldb_2026` 分支、100 分制和“周六下午四点截止”等描述，和本届专用页面不一致。本届按 `2026-competition` 分支、620 分制、群公告截止时间及本届平台执行，不套用旧页面的提交流程和成绩保留规则。

来源：[判题时效](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/cedao0wehhx1gcg4)、[旧测试注意事项](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/yt95uatolcbtfgow)。

## 当前项目与交付状态

- 当前 seekdb 工作目录为 `/Users/liwangsheng/.codex/worktrees/issue-1477/oceanbase`；原目录 `/Users/liwangsheng/project/oceanbase` 保留私有测试产物，PowerContext 位于其 `.competition/powercontext`。
- 用户远端为 `git@github.com:iris82681414-beep/seekdb2026.git`。2026 年 10 月 27 日经 GitHub 接口重新核验为私有仓库；两套产品均已保留官方历史并推送私有答题分支。
- seekdb 使用私有 `2026-competition` 分支；PowerContext 使用私有 `powercontext-2026-competition` 分支，后者也是仓库当前默认分支。正式提测应明确选择产品对应的分支与 commit，不能直接依赖默认分支。
- 同日重新核对官方 `2026-competition`：seekdb 为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`，PowerContext 为 `dab240063bfcfa7917898dc44da4baadb3747bd0`，与采用的赛题基线一致。
- 用户要求暂缓 AI 生态题、正式提测及公开上传。私有导入仓库没有 GitHub Fork 关系，不满足公开 Fork 子题；指定 Bug #1477 的独立私有修复也尚未提交官方 PR。
- 已实现内容、各题证据与剩余门槛以 [实施进度](competition-progress.md) 为准。当前没有平台确认分数，公开回归和独立测量不能折算成正式成绩。

官方基线：[seekdb 比赛分支](https://github.com/oceanbase/seekdb/tree/2026-competition)、[PowerContext 比赛分支](https://github.com/oceanbase/powercontext/tree/2026-competition)。
