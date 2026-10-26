# 2026 OceanBase 初赛实施进度

大赛总分 620。按用户最新要求，当前只推进 seekdb 内核与 PowerContext；AI 生态题暂缓，正式提测和公开上传暂缓。尚无平台确认得分；公开测试和独立测量不等于正式成绩。

## 环境与提交

- seekdb 保留官方 `2026-competition` 历史，基线为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。用户私有仓库已建立 `main` 和 `2026-competition` 分支。
- PowerContext 位于本地 `.competition/powercontext`，保留比赛分支及冻结产品提交 `9db1753434162c0aa7c9f1189a1722e617880a52`。
- PowerContext 最新实现已推送至私有仓库 `powercontext-2026-competition`，完整提交为 `94474fecb3bc772713e0fb6ca3c28d92ebef22ea`。
- GitHub 仓库当前默认分支仍是 PowerContext 答题分支；正式提测须明确选取产品对应的分支和 commit。
- 华为云现有实例为 Ubuntu 24.04 x86_64、4 核、约 15 GB 内存。专用目录 `/root/oceanbase2026`，日志位于 `logs`，测试产物位于 `results`。
- 连接信息保存在仓库外 `/Users/liwangsheng/.config/oceanbase2026/server.env`，文件权限 600、目录权限 700。额外源码、下载数据和操作脚本均由本地排除规则排除，未提交到 Git。

## 实现与验收

| 项目 | 分值 | 已取得的证据及剩余事项 |
| --- | ---: | --- |
| seekdb 基础实践 | 30 | 原生实例已部署；telemetry 已发送。公开 Fork、官方 Bug PR、本队验证行和平台核验待完成 |
| AI_PROMPT | 10 | JSON、TEXT/LOB 和重复占位符实现已编译；原版完整 mysqltest 及新增真实 HTTP 回归通过 |
| AI_EMBED 单行、AI_COMPLETE | 20 | 请求/响应核心实现通过长文本、空配置、JSON 配置、维度及 17 组异常响应的原生回归，待正式完整验收 |
| Ollama Provider | 10 | 原生免 Key、可选认证、生成、参数和向量路径回归通过；同轮 12 项 mysqltest 全通过 |
| AI_RERANK、跨行批量 AI_EMBED | 20 | 原生 SQL 实现及回归已通过；重排跨三批完整排序、14 类异常响应；向量按模型/维度分组、NULL/LOB、乱序响应和实际合并请求已验证 |
| 文件即数据库 | 50 | 原生三格式读取、批量/重扫、DESCRIBE、glob、CSV COPY 已实现；原版及新增 2 项 mysqltest、8 项 Rust 测试、30 项独立 SQL 专项通过，隐藏边界继续核验 |
| AI 列 | 50 | DDL、事务任务、后台计算和回填已实现；7 项 AI COLUMN 及 12 项 AI 函数回归、15 项后台独立检查和实例中断恢复通过；Strict Read 与手动 RETRY 继续实现，尚未完成该题 |
| 全文索引性能 | 50 | 冻结原版二进制已完成原版默认规模测量、命中一致；本机按固定 CI 基线计算已为 100/100，不作为本实现的优化收益或正式成绩 |
| PowerContext 仓库准备 | 10 | 冻结历史和私有分支已准备，待平台读取配置及核验 |
| CSV 行级采集 | 20 | Linux 公开验收 17 项通过 |
| Pi 会话采集 | 20 | Linux 公开验收 15 项通过 |
| 财报 PDF | 50 | 全部 503 页公开样例及 6 项 Linux 验收通过；正式解析质量及 Recall@20 待验证 |
| HTTP 重排 | 20 | Linux 公开验收 10 项通过 |
| 上下文增量传输 | 20 | Linux 公开验收 14 项通过；另已完成真实 QMSum 40 轮独立成本测量，正式序列待验证 |
| 数量触发记忆处理 | 20 | Linux 公开验收 17 项通过，覆盖单窗、数量/时间 OR、重复身份、lineage、重启及故障恢复 |
| Embedding 合并和缓存 | 20 | Linux 公开验收 8 项通过，包含六个 scope 的四次请求预算 |
| 来源撤回与恢复 | 20 | Linux 公开验收 4 项通过；另有原生存储失败后跨重启恢复及 scope 隔离回归通过 |
| 预算内证据选择 | 20 | Linux 公开验收 3 项通过；48 次请求的完整质量门槛待验证 |
| seekdb 单 SQL 混合检索 | 20 | 原生 seekdb Linux 验收通过，含两类索引 EXPLAIN、过滤、版本和停用恢复；完整质量与延迟门槛待验证 |
| 高德地图 Skill | 20 | 已有单文件与格式校验记录；按用户要求暂缓，不再推进 |
| 其余六道生态题 | 120 | 按用户要求暂缓 |

PowerContext 的十道代码题在官方固定依赖、CPUQuota 100% 和 MemoryMax 2G 限制下，共 95 项原版 Linux 公开测试全部通过，覆盖题面分值合计 230 分。原版测试、依赖锁文件、工作负载和评分文件保持原样。日志为本地 `.competition/results/powercontext-complete.log` 与远端 `logs/powercontext-complete.log`，退出码 0。

独立增量测量使用固定 QMSum commit 的 ES2004a 原文，SHA-256 为 `911e56db0d8482f1e730bae0a8fcf0ab0095c94abb000ba0516e975fa6e48b31`。初始 8 个真实 turn，40 轮中追加 3 个 turn 并切换查询，形成 8 种不同 Context；每轮均经序列化独立重建并逐字核对。原始完整正文累计 150,035 bytes，增量协议累计 47,768 bytes，净节省 68.16%；完整 v1 pack 累计 208,255 bytes，逐轮 gzip 累计 67,115 bytes。该独立序列不是平台正式成本序列，结果保存于 `.competition/results/context-pack-measurement.json`。

产品修改通过针对性 Ruff 和固定版本 ty 检查，75 项上下文与生命周期回归通过。全量开发钩子仍报告冻结公开样例的格式问题和测试类型诊断；钩子对原版样例的自动修改已恢复，未用修改测试的方式绕过验收。原始锁文件离线一致性检查通过。

seekdb 官方基线、AI_PROMPT 及后续请求/响应修改均已在服务器构建成功。新增请求验证覆盖 50,000 字符的真实 LOB 读入；17 组异常响应通过 loopback HTTP 返回，SQL 均拒绝异常输入且实例继续正常响应。基线可执行文件另存于 `/root/oceanbase2026/seekdb-baseline`。实例部署标识为 `becf1a63-c20a-8684-a088-a2f704ce1127`，来自实例 `run/telemetry.json` 的 `content.id`，`sent=true`；平台同步和核验仍待正式操作。

AI 函数增强的六项实现已覆盖。Linux 原生实例使用未修改的 `ai_function_enhancement` 综合用例与其他原版回归，以及新增的重排、批量向量等用例，共 12 项 mysqltest 通过。HTTP 响应由本地确定性协议 fixture 生成，未使用平台 Mock 或真实在线模型；该记录不替代平台隐藏测试。完整结果位于 `.competition/results/seekdb-ai-complete.json`。Endpoint 的 `max_concurrency` 已补齐 1..1024 整数校验、默认值 8、系统表存储、视图和复制语义；在全新实例上，包含原版 Endpoint DDL 及新增边界回归的 12 项 mysqltest 全通过。该修改只交付参数存储，AI COLUMN 后台实际限流尚待实现。

全文索引冻结基线使用独立实例和原版 benchmark，配置为 20,000 行、分词 3,000 轮、查询 200 轮、3 个样本和 30 轮预热。四类查询命中为 8001、11000、7332、20，与固定基线一致，配置差异为空。原始二进制相对仓库内固定 CI 基线的内部报告已为 100/100，说明该分数受本机环境影响，不能据此宣称产品优化或正式 50 分。测量保存于 `.competition/results/fts-baseline-score.json`，后续性能修改仍须同机前后比较。

新增边界验证还覆盖 OpenAI、Ollama、Dashscope 的空 Completion，以及 Dashscope 错误 JSON 结构，均按错误返回。跨行向量回归包含过滤、倒序和 LIMIT，以及 1,100 行跨执行批次、35 个 NULL 的原始行映射；同轮 12 项用例通过，另有 17 组异常响应被拒绝且实例保持可用。该二进制运行于新建测试实例 `/root/oceanbase2026/instances/core-endpoints`，原始实例及冻结二进制继续保留。

预算证据题已增加独立真实 SciFact 质量测量。固定下载包 SHA-256 为 `11c621288d41ac144d29b13b0f8503b3820b7d6e8b1f6ff24dff335c196d76be`；从 dev claim 池按固定种子选取 24 条、每条三个摘要，两档预算共 48 次请求，并检查标注片段连同实际引用及 envelope 均能装入预算。所有候选通过本机 SQLite Runtime 真实入库、选择和引用回查；标注仅用于独立计分，不传入产品。平均证据词项召回为 62.27%，完整覆盖为 41.67%。这不是正式序列，且独立样本的召回仍低于题面阈值，因此该题尚不能判定质量达标。结果位于 `.competition/results/evidence-selection-feasible-48.json`；未保留缺少稳定收益的排序调整，PowerContext 产品提交仍为上列已验证版本。


File SQL 已实现 CSV、JSONL、Parquet 的原生读取，SQL 命名选项、别名和动态类型，包含 DESCRIBE、文件发现、批量取数、重扫、聚合及 JOIN。COPY 支持表或查询输出、表头与 CSV 选项，目标已存在时拒绝覆盖；空查询也会发布表头，查询失败会清理临时文件。Linux 原版 `file_sql_basic` 和新增 `file_sql_queries` 两项 mysqltest 均通过；原版用例及原始数据保持原样。结果保存于 `.competition/results/seekdb-file-sql-final.json`。

格式与编码层的 8 项 Rust 测试、格式检查和 Clippy 全目标检查通过，日志位于 `.competition/results/file-scan-quality.log`。原生 SQL 另外通过 10 项权限专项和 11 项大文件及失败路径专项：覆盖旧连接实时遵守全局目录规则、NULL/空规则、目录前缀与符号链接、FILE 权限及启用角色、跨 2,005 行批次、分组、跨格式 JOIN、LEFT JOIN 补 NULL、全 NULL 列，以及 300 行合计 19,800,000 bytes 的字段处理。中途修改输入文件会中止 COPY；并发抢先创建的目标保留原内容，临时文件清理验证通过。两份记录为 `.competition/results/seekdb-file-sql-policy.json`、`.competition/results/seekdb-file-sql-streaming.json`。这些是自建数据的独立测试，不替代隐藏测试或平台成绩。

File SQL 该轮测试实例为 `/root/oceanbase2026/instances/core-files`，验证二进制另存为 `/root/oceanbase2026/seekdb-file-sql-empty`。整合 File SQL 后，既有 12 项 AI mysqltest 再次全部通过，记录为 `.competition/results/seekdb-ai-after-file-sql.json`，模型响应仍来自本地确定性 HTTP fixture。新增系统变量的生成器已验证重复生成一致，原有 719 个变量的赋值保持一致；增量构建会跟踪语法源文件并重新生成解析器。文件发现和类型边界仍继续做专项复核。

文件发现进一步改为逐目录扫描，限制保留 10,000 个路径、检查 100,000 个目录项及路径，并周期性检查查询取消状态，避免系统 glob 在检查上限前先分配所有匹配项。新增 9 项独立检查全部通过，包括目录通配、合法目录链接、隐藏文件、无匹配、10,001 个匹配文件及 100,001 个不匹配文件的超限路径。连续 30 次超限后，实例文件句柄数保持 25；同轮两项 File SQL 综合用例和 10 项权限专项再次通过。记录为 `.competition/results/seekdb-file-sql-glob.json`、`.competition/results/seekdb-file-sql-bounded.json` 和上述权限记录。该轮实例使用 `/root/oceanbase2026/seekdb-file-sql-bounded` 的副本，其 SHA-256 为 `7b2a339bcab4a88b7bfae6fea6943af6eb9bfad55d1b414b8c6791310bf4e45c`。

AI COLUMN 的 DDL 阶段已完成原生实现，定义作为列元数据持久保存，提供目录视图 `__all_ai_column`，并建立后续使用的持久化任务表。每个 AI 列具有三个独立隐藏列；修改定义会复用这些列并更新定义版本，删除列时清理活动元数据。原版 `ai_column_ddl` 和新增 `ai_column_ddl_changes` 两项 mysqltest 通过；27 项独立检查覆盖普通字段前向引用、普通列转 AI 列、批量添加、修改参数、删除依赖保护，以及非法选项、嵌套 AI、AI 列依赖和维度不一致等拒绝路径。实际停止并重启实例后，修改过的定义、SHOW CREATE 和隐藏列归属逐项一致。记录为 `.competition/results/seekdb-ai-column-ddl.json`、`seekdb-ai-column-ddl-independent.json` 和 `seekdb-ai-column-restart.json`。

当前原生实例为 `/root/oceanbase2026/instances/core-ai-column`，管理路径为 `/root/oceanbase2026/seekdb-ai-column-ddl`，其验证副本 `seekdb-ai-column-ddl-modify` 的 SHA-256 为 `ece2d0ec169304d3df3422a1452fa062bbb553dda45708d3572c7533588f9b48`。此前的 `core-files` 保留并停机。该版本的既有 12 项 AI 函数 mysqltest 再次全部通过，记录为 `.competition/results/seekdb-ai-after-ai-column-ddl.json`；AI 响应仍为本地协议 fixture。此阶段尚未验证 后台模型调用、Strict Read、手动重试和向量索引维护，不宣称 AI 列题完成或取得 50 分。

AI COLUMN 的业务写入现已在同一事务内清空受影响结果、更新隐藏状态并写入持久化任务；同一行受影响的列共享请求标识，无关字段更新保留结果与任务。12 项独立 SQL 检查全部通过，覆盖提交、回滚、主键变更、批量插入与删除、显式结果赋值拒绝和隐式列清单。新增 `ai_column_transactions` 回归进一步验证语句失败、保存点及整事务回滚，以及 REPLACE、冲突更新、INSERT SELECT、联表更新、二进制和微秒时间戳主键。不依赖在线模型；两项既有 DDL 和 12 项 AI 函数 mysqltest 同轮通过。记录为 `.competition/results/seekdb-ai-column-t1-independent.json`、`seekdb-ai-column-t1-regression.json` 和 `seekdb-ai-after-ai-column-t1.json`。

AI COLUMN 后台计算已接入原生服务生命周期，支持持久任务认领、实际 Endpoint 并发限制、重试、过期任务隔离和同一行结果的事务发布。新增或修改 AI 列会对已有数据分批计算，使用主键游标避免反复扫描已处理的行。输出通过原生类型和精度检查；唯一索引或检查约束拒绝某列时，该列结束为失败，其余有效列仍在同一事务中发布。原版 DDL、T1、Worker、向量索引用例及新增 DDL、事务、回填回归，共 7 项 AI COLUMN mysqltest 全部通过；同轮 12 项 AI 函数回归全部通过。新增回填用例覆盖 65 行、同组原子可见、修改定义重算、AI_PROMPT 与普通文本区分、请求配置保护、约束失败收尾及无显式主键的重复行。

15 项独立后台检查全部通过，包括两个共享 URL 的 Endpoint 实测并发峰值分别为 2 和 1、非法整数/日期/JSON/超长文本/数值溢出收尾，以及二进制、UTF-8 和时间戳主键的写入及回填。另一次真实测试实例中断验证在 HTTP 请求进行中发送 SIGKILL，重启后 7.958 秒完成租约恢复和任务清理；这些是自建协议 fixture 的结果，不是平台提测。记录为 `.competition/results/seekdb-ai-column-worker.json`、`seekdb-ai-column-worker-independent.json`、`seekdb-ai-column-worker-restart.json` 和 `seekdb-ai-after-ai-column-worker.json`。两项 File SQL 回归同轮通过，记录为 `.competition/results/seekdb-file-after-ai-column-worker.json`。当前验证副本为 `/root/oceanbase2026/seekdb-ai-column-worker-constraints2`，SHA-256 为 `8e45e4ce0d6c09ea30b06b0e6bf82fbf418d7b651abd6f058fab7cc2d05814a3`；管理路径继续为 `seekdb-ai-column-ddl`。Strict Read 和手动 RETRY 仍待实现，不宣称 AI 列题完成。

## 后续工作

1. 完成 File SQL、AI COLUMN 和 FTS 性能任务，并继续覆盖 AI 函数隐藏边界，使用原版测试验证。
2. 继续核验 PowerContext 的完整质量及成本门槛，不能把接口用例通过视为满分。
3. AI 生态题暂缓，保留已有私有材料。
4. 正式提测暂不执行。保留可复现交付物和测试记录，后续按用户指示安排。
