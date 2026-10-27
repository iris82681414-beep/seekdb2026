# 2026 OceanBase 初赛实施进度

大赛总分 620。按用户最新要求，当前只推进 seekdb 内核与 PowerContext；AI 生态题暂缓，正式提测和公开上传暂缓。尚无平台确认得分；公开测试和独立测量不等于正式成绩。

## 环境与提交

- seekdb 保留官方 `2026-competition` 历史，基线为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`。用户私有仓库已建立 `main` 和 `2026-competition` 分支。
- seekdb 最新内核实现提交为 `3c2738049ea434300517356249ee4bbe897861bb`，包含 FTS 锁优化、大赛指定 Bug #1477、TOKENIZE 大字段及空配置崩溃修复。独立 Bug 分支提交为 `34b696a50198bdb8d5280f3f881ede6405d96625`，基于官方 master，保存在用户私有仓库。
- PowerContext 位于本地 `.competition/powercontext`，保留比赛分支及冻结产品提交 `9db1753434162c0aa7c9f1189a1722e617880a52`。
- PowerContext 最新实现已推送至私有仓库 `powercontext-2026-competition`，完整提交为 `a06ae43ae42fdd362485a7b0de4552ad10e53436`。
- GitHub 仓库当前默认分支仍是 PowerContext 答题分支；正式提测须明确选取产品对应的分支和 commit。
- 华为云现有实例为 Ubuntu 24.04 x86_64、4 核、约 15 GB 内存。专用目录 `/root/oceanbase2026`，日志位于 `logs`，测试产物位于 `results`。
- 当前原生主实例为 `/root/oceanbase2026/instances/core-ai-column-strict`，SQL 端口 2881；运行二进制 SHA-256 为 `970e2a68101a5053602fb95b130d3fce945dcd4e545f544bbc95f804f719042d`。部署标识文件中的 ID 为 `8df70a0e-f2ad-8202-8b3b-ad658ab165d9`，正式部署核验须从该实例重新读取并核对平台同步结果。
- 连接信息保存在仓库外 `/Users/liwangsheng/.config/oceanbase2026/server.env`，文件权限 600、目录权限 700。额外源码、下载数据和操作脚本均由本地排除规则排除，未提交到 Git。

## 实现与验收

| 序号 | 项目 | 题面分值 | 私有证据及剩余事项 |
| ---: | --- | ---: | --- |
| 1 | 部署 | 10 | 实例运行，部署标识已保存；待平台同步与核验 |
| 2 | 公开 Fork | 10 | 按要求暂缓；私有导入仓库不具备 Fork 关系 |
| 3 | 官方 Bug PR | 10 | #1477 独立修复及 576 组对照完成；官方 PR 暂缓 |
| 4 | AI_PROMPT | 10 | 原版及 TEXT/LOB、JSON、占位符回归通过 |
| 5 | AI_EMBED 单行 | 10 | 长文本、配置、维度及异常响应原生回归通过 |
| 6 | AI_COMPLETE | 10 | 长文本、配置及异常响应原生回归通过 |
| 7 | Ollama Provider | 10 | 免 Key、可选认证、生成和向量原生回归通过 |
| 8 | AI_RERANK | 10 | 三批全局排序、稳定顺序与异常响应回归通过 |
| 9 | AI_EMBED 跨行批量 | 10 | 分组、NULL/LOB、乱序响应及 1,100 行映射回归通过 |
| 10 | 文件即数据库 | 50 | 原版及新增 2 项 mysqltest、8 项 Rust 测试及 SQL 专项通过 |
| 11 | AI 列 | 50 | 五组实现及 10 项回归通过，独立事务、权限、索引检查通过 |
| 12 | 全文索引性能 | 50 | 锁优化同机对照和正确性通过；LOB、NULL 崩溃修复通过，性能满分未证实 |
| 13 | 仓库准备 | 10 | 冻结历史和私有分支具备；待平台读取与核验 |
| 14 | CSV 行级采集 | 20 | Linux 原版 17 项通过 |
| 15 | Pi 会话采集 | 20 | Linux 原版 15 项及新增 9 项回归通过，拒绝整个归档中的非法 JSON 常量 |
| 16 | 财报 PDF | 50 | 原版及新增 18 项通过；12,013 页解析及独立召回验证，冻结字数待核验 |
| 17 | HTTP 重排 | 20 | Linux 原版 10 项通过；正式冻结向量质量门槛待核验 |
| 18 | 上下文增量传输 | 20 | Linux 原版 14 项通过；独立 QMSum 40 轮净节省 68.16% |
| 19 | 数量触发记忆处理 | 20 | Linux 原版 17 项通过，含时间 OR、lineage、重启和失败恢复 |
| 20 | Embedding 合并和缓存 | 20 | Linux 原版 8 项通过，含六个 scope 的四次请求预算 |
| 21 | 来源撤回与恢复 | 20 | Linux 原版 4 项通过，另有失败跨重启及 scope 隔离回归 |
| 22 | 预算内证据选择 | 20 | 原版 3 项通过；独立 48 请求召回 68.08%、完整覆盖 64.58% |
| 23 | seekdb 单 SQL 混合检索 | 20 | 原生验收通过；独立 60 次平均召回 95%、逐查询无退化 |
| 24 | AMD | 20 | 按用户要求暂缓 |
| 25 | MiniMax | 20 | 按用户要求暂缓 |
| 26 | Qoder | 20 | 按用户要求暂缓 |
| 27 | 阶跃星辰 | 20 | 按用户要求暂缓 |
| 28 | Dify | 20 | 按用户要求暂缓 |
| 29 | 高德地图 | 20 | Skill 格式已校验并私有保留；按用户要求暂缓 |
| 30 | 魔搭 | 20 | 按用户要求暂缓 |

2026 年 10 月 27 日已按上述 30 项复核，总计 620 分。19 道代码题涉及题面分值 440 分，公开回归和独立测量已有记录；部署与仓库准备涉及 20 分，公开 Fork/PR 涉及 20 分，AI 生态涉及 140 分。以上是题目覆盖范围，正式成绩未知，不能记作已得分。

PowerContext 的十道代码题已在完整提交 `a06ae43ae42fdd362485a7b0de4552ad10e53436`、官方固定依赖、CPUQuota 100% 和 MemoryMax 2G 配置下，完成全部 95 项原版回归及 9 项新增 Pi 回归，无测试失败或跳过。原版分项数量为 CSV 17、Pi 15、PDF 6、HTTP 重排 10、增量上下文 14、数量触发 17、Embedding 8、来源生命周期 4、预算证据 3、原生混合检索 1，涉及题面分值合计 230 分。原版测试、依赖锁文件、工作负载和评分文件保持原样；86 个冻结输入文件与官方基线相同。测试前后逐文件核对全部 430 个产品源码文件，无差异、无额外源码，源码清单 SHA-256 为 `6e632efb6164d40d629ef25d0a498c3b1f4538d417bda127bb5e3a91c01b1d09`。验证依据源码摘要，不把服务器旧 Git HEAD 当作候选版本。原生用例使用固定 `pylibseekdb==1.3.0.post4` 嵌入式实例，与单独运行的 seekdb 主实例分开。

本次执行先通过前九组 94 项，最后一组因验证脚本的临时目录过长而等待：109 字节的 Unix socket 绝对路径被拒绝，同一实例以短相对路径可连接。确认原因后停止该次验证，保留日志，仅缩短临时目录，单独重跑最后一组和新增回归，分别为 1 项、9 项通过，退出码均为 0。产品与冻结输入未变。前 94 项日志为 `.competition/results/powercontext-pi-json-path-setup-error.log`，后续通过日志为 `powercontext-pi-json-public.log`，汇总为 `powercontext-pi-json-validation.json`；原先 `5c7e7450` 的完整 95 项日志继续保留于 `powercontext-final-public.log`。

Pi 修复拒绝 Python 默认 JSON 解析器额外接受的 `NaN`、`Infinity`、`-Infinity`，包括会话头和未选中分支中的非法值，满足题面“校验整个归档”的要求。旧版在新增测试中为 6 项失败、3 项通过，修改后 9 项全部通过；合法的同名字符串和普通数值保持可读。修改文件的 Ruff、格式和针对性 ty 检查通过。记录为 `.competition/results/powercontext-pi-json-before.log`。公开用例通过仍不等于平台正式得分。

## 尚待正式核验的门槛

所有代码题仍需平台隐藏测试；PowerContext 题目按全项满足判定，公开用例数量不能按比例换算分数。下面保留尚未证实的主要门槛：

| 项目 | 正式门槛与当前证据边界 |
| --- | --- |
| 部署、仓库、公开材料 | 部署 UUID 须与平台同步；私有仓库授权、产品分支和完整 commit 须核验。公开 Fork 的 README 和官方 Bug PR 正文分别需要本队验证行，均按用户要求暂缓 |
| PDF | 正式五份财报共 1,151 页、24 问，各组 Recall@20 至少 70%；每个物理页词项数为冻结值的 90%–110%，每条证据召回和标注整页 F1 均至少 95%。独立 84 份年报测量不能替代未公开的冻结计数与分组 |
| HTTP 重排 | 173 篇摘要、20 问、冻结 384 维 MiniLM 向量，batch_size=7；Recall@10 至少 95%、MRR@10 至少 80%，还须符合真实服务分数的全局稳定排序。公开协议测试尚不证明该冻结质量序列 |
| 增量、预算证据 | 正式增量 40 轮相对原始完整正文至少节省 45%；预算证据 48 次请求平均召回至少 65%、完整覆盖至少 40%。现有 QMSum 和 SciFact 独立测量达到对应门槛，但输入序列与平台不同 |
| 混合检索 | 正式 60 次查询须逐条 Recall/MRR 不低于同库冻结双查询路径，平均召回至少 95%、完整搜索延迟中位数至多 500 ms，并有单 SQL 与双索引证据。现有独立测量使用文本特征向量，未替代平台冻结向量 |
| 全文索引 | 构建、分词和查询三类提升率等权平均，综合耗时下降 50% 才是内部报告满分。固定 CI 报告在修改前已达 100/100；现有同机优化幅度不证明正式 50 分 |
| AI 生态 | 七题共 140 分，按用户要求暂缓；现有单文件 Skill 材料没有公开或正式提交 |

逐项复核记录保存在本地 `.competition/results/competition-readiness-audit-20261027.json`；正式分数字段保留为空。

## 分阶段验证记录

以下保留各阶段的提交、实例和原始测量，便于复现；旧阶段的实例路径、部署标识和未实现项不代表当前状态，当前版本以“环境与提交”和最新整体验证为准。本次仅改动 Pi 解析，先前 PDF、质量和性能测量仍标注其实际测量版本，不冒充重测结果。

独立增量测量使用固定 QMSum commit 的 ES2004a 原文，SHA-256 为 `911e56db0d8482f1e730bae0a8fcf0ab0095c94abb000ba0516e975fa6e48b31`。初始 8 个真实 turn，40 轮中追加 3 个 turn 并切换查询，形成 8 种不同 Context；每轮均经序列化独立重建并逐字核对。原始完整正文累计 150,035 bytes，增量协议累计 47,768 bytes，净节省 68.16%；完整 v1 pack 累计 208,255 bytes，逐轮 gzip 累计 67,115 bytes。该独立序列不是平台正式成本序列，结果保存于 `.competition/results/context-pack-measurement.json`。

产品修改通过针对性 Ruff 和固定版本 ty 检查，75 项上下文与生命周期回归通过。全量开发钩子仍报告冻结公开样例的格式问题和测试类型诊断；钩子对原版样例的自动修改已恢复，未用修改测试的方式绕过验收。原始锁文件离线一致性检查通过。

seekdb 官方基线、AI_PROMPT 及后续请求/响应修改均已在服务器构建成功。新增请求验证覆盖 50,000 字符的真实 LOB 读入；17 组异常响应通过 loopback HTTP 返回，SQL 均拒绝异常输入且实例继续正常响应。基线可执行文件另存于 `/root/oceanbase2026/seekdb-baseline`。初始部署实例的标识为 `becf1a63-c20a-8684-a088-a2f704ce1127`，来自实例 `run/telemetry.json` 的 `content.id`，`sent=true`；平台同步和核验仍待正式操作。

AI 函数增强的六项实现已覆盖。Linux 原生实例使用未修改的 `ai_function_enhancement` 综合用例与其他原版回归，以及新增的重排、批量向量等用例，共 12 项 mysqltest 通过。HTTP 响应由本地确定性协议 fixture 生成，未使用平台 Mock 或真实在线模型；该记录不替代平台隐藏测试。完整结果位于 `.competition/results/seekdb-ai-complete.json`。Endpoint 的 `max_concurrency` 已补齐 1..1024 整数校验、默认值 8、系统表存储、视图和复制语义；在全新实例上，包含原版 Endpoint DDL 及新增边界回归的 12 项 mysqltest 全通过。该阶段仅验证参数存储；后台实际限流的后续验证见下文 AI COLUMN Worker 记录。

全文索引冻结基线使用独立实例和原版 benchmark，配置为 20,000 行、分词 3,000 轮、查询 200 轮、3 个样本和 30 轮预热。四类查询命中为 8001、11000、7332、20，与固定基线一致，配置差异为空。原始二进制相对仓库内固定 CI 基线的内部报告已为 100/100，说明该分数受本机环境影响，不能据此宣称产品优化或正式 50 分。测量保存于 `.competition/results/fts-baseline-score.json`，后续性能修改仍须同机前后比较。

FTS 锁优化只改变每次分词独占的词频表，统一优化器、当前 DAS 和 legacy 查询初始化中的表类型；全局停用词、词典缓存与节点分配器的同步方式保持原样。以已包含 AI COLUMN、File SQL 等实现的提交 `408c7ac8` 为直接对照，使用同一管理实例和相同配置，按“对照—修改—对照”执行未改动的原版默认负载：20,000 行、分词 3,000 轮、每条查询 200 轮、3 个样本及 30 轮预热。进程配置为 memory_limit 4G、log_disk_size 4G；计时期间使用 CPUQuota 400%、MemoryMax 5G，未与编译或 PowerContext 质量测量重叠。以两次对照均值比较，中文分词从 0.07805 ms 降到 0.0718 ms（8.01%），英文从 0.03775 ms 降到 0.0360 ms（4.64%）。三类索引构建分别观测下降 1.52%、3.01%、3.83%；查询变化为下降 0.39% 至 1.78%，其中中文查询观测退化 1.14%，不宣称查询已获得稳定的整体加速。

四类查询仍命中 8,001、11,000、7,332、20；三轮分别完成 128 次并发结果核对，共 384 次，中文/英文词项与词频、全文查询结果全部一致。原版 basic_dml、cn_word、parser_properties、simple_query、tokenize_function 五项用例在对照与修改二进制上均通过。构建和原版测试通过后保存快照 `/root/oceanbase2026/seekdb-fts-local-map-candidate`，SHA-256 为 `e6be5e0b3adebd43a35905d9e1dab459aa03ac08ef7577b58c10ad5deeafdc31`。对照二进制为已有最终 AI COLUMN 快照 `dea2203f…`；早先的冻结旧二进制仅用于定位热点，不混入本次单项收益对照。原版工作负载、SQL、计时、评分与固定基线文件未改动，三轮相对固定 CI 的内部报告均为 100/100；本机已达到该报告上限，不能据此宣称正式 50 分。记录为 `.competition/results/seekdb-fts-local-map-validation.json` 与 `.competition/fts-quality/current-comparison/`。

新增边界验证还覆盖 OpenAI、Ollama、Dashscope 的空 Completion，以及 Dashscope 错误 JSON 结构，均按错误返回。跨行向量回归包含过滤、倒序和 LIMIT，以及 1,100 行跨执行批次、35 个 NULL 的原始行映射；同轮 12 项用例通过，另有 17 组异常响应被拒绝且实例保持可用。该二进制运行于新建测试实例 `/root/oceanbase2026/instances/core-endpoints`，原始实例及冻结二进制继续保留。

预算证据选择已调整候选文档频率权重，降低所有摘要共同出现的词对排序的影响，保留查询特有证据。SciFact 固定下载包 SHA-256 为 `11c621288d41ac144d29b13b0f8503b3820b7d6e8b1f6ff24dff335c196d76be`。原开发样本 48 次请求的平均证据词项召回从 62.27% 升到 65.80%，完整覆盖从 41.67% 升到 45.83%。另以种子 20261028 排除此前使用过的 75 条 claim，建立未参与调整的样本；从 40 条 claim 的 80 次测量中按证据预算可行性选取 24 条、两档预算共 48 次计分请求，每次仍包含三篇原始摘要。基线和修改版输入逐项相同；召回从 63.65% 升到 68.08%，完整覆盖从 56.25% 升到 64.58%，超过题面 65% 和 40% 的门槛。全部候选经真实 SQLite Runtime 入库、选择和引用回查；标注仅用于外部计分，不传入产品。该序列仍不替代平台正式质量验收。

同一新样本在 Linux 官方固定依赖、单核 CPU 配额和 2 GiB 内存限制下复现相同质量值，80 次测量池耗时 6.26 秒、内存峰值 217,664 KiB。Linux 的 3 项原版证据用例、14 项增量上下文用例及 3 项自有证据回归通过；本地相关测试共 74 项通过。新增回归证明重复通用词不能挤掉文档尾部的区分性证据，旧实现失败、修改版通过。修改文件的格式、lint 和类型检查通过；全仓类型检查仍有与未修改基线输出完全相同的 190 条诊断，未宣称全仓类型检查通过。记录为 `.competition/results/powercontext-evidence-idf-validation.json`、`evidence-selection-idf-linux.json`、`evidence-selection-idf-linux-baseline.json` 和 `powercontext-evidence-idf-acceptance.log`。

PDF 扩展验证使用 FinanceBench 固定 revision `cc39aeb4afdf33909ee1412188bf89035950c2eb`，下载并逐文件校验其 150 个公开问题涉及的 84 份完整 PDF，约 165.5 MB。解析器按坐标重建文本行，处理字体映射为换行符或不换行空格的间隔字形；若版面提取与原提取的非空白字符多重集合不一致，保留原提取结果。Linux 在官方固定依赖、单核和 2 GiB 限制下，完整解析 12,013 个物理页，耗时 1,264.18 秒、内存峰值 188,876 KiB。189 条标注的证据词项召回和整页 F1 均达到 95% 门槛，最低值分别为 97.48% 和 96.27%。原版 6 项和新增 12 项 PDF 回归共 18 项通过，覆盖特殊字体映射、旋转文本、普通与连写报表标题、财务概念、公司主体及报告期间的精确引用。检索回归在 SQLite 与原生 seekdb 两种后端实际执行，验证关闭重启后的完整正文与精确 citation；旧实现会在新增回归中失败。平台未公开的非标注页冻结词项数仍不能核对，不将原始 pypdf 的词项数当成平台冻结值。这是更广公开语料上的独立研究，不等于复现平台未公开的五份文档组合。

完整页面仍有 12 页的规范化文本投影超过 8,192 字节，最大为 9,209 字节。带有已校验 Source 引用的 Memory 条目现支持最多 65,536 字节，独立手工笔记仍限制为 8,192 字节；保存、搜索、修订和历史版本回读均使用同一规范化及内容摘要规则，不裁掉正文。SQLite 与原生 seekdb 的关闭重启、重复提交、尾部全文检索、修订和旧引用回查均通过。相关本地测试 114 项通过、14 项按环境条件跳过；Linux Memory 相关测试 89 项通过、13 项按环境条件跳过，包含原生 seekdb 实测。API 已从契约重新生成，49 项契约测试通过。记录为 `.competition/results/powercontext-source-capacity-local.log`、`powercontext-source-capacity-linux.log` 和 `powercontext-long-evidence-api.log`。

扩展检索按公司分成 32 个独立数据范围，150 个问题全部完成真实 Runtime 入库、重启和逐条引用回查，无失败组。公司主体、报告期间、财务报表及业务分部的覆盖均使用原问题与页面内容；客户问题补充集中度、合同和销售表述，库存周转率同时保留收入表和资产负债表，账面价值及清算问题可检索 BVPS、TBVPS 等常见缩写。业绩公告排序只使用调用者文档 ID 中的报告类型词。产品不读取标签或公司分组清单，不包含公司、题目、页面或答案映射。

在同一 Linux 数据库、相同 scope 和已入库身份上，与直接父提交 `7f6d0719` 对照，库存与每股账面价值覆盖使 Recall@20 从 165/187（88.24%）提高到 169/187（90.37%），3 个问题增加命中，没有问题减少命中。32 个公司组均超过 70%；此前不足的 JPMorgan 为 4/5、Johnson & Johnson 为 8/10。复用数据库的检索测量耗时 183.61 秒、内存峰值 167,712 KiB，搜索延迟中位数为 931.86 ms。本地相关测试 7 项通过、5 项因缺少原生 seekdb 跳过；Linux 固定依赖下原版与自有 PDF 回归共 18 项全部通过，包含五项原生 E2E 用例。新增比率与缩写断言在旧实现的普通、连写报表标题场景中均失败，修改后通过；财务报表、公司主体、报告期间、重启后的精确 citation 和完整正文回查继续通过。格式、lint、针对性类型检查及修改文件钩子通过；未宣称全仓类型检查已消除既有诊断。

另从初始开发范围按问题数选择五家不同公司的完整年报，建立包含 1,322 页和 26 个原始问题的独立联合范围。相同数据库、相同已入库身份上，最新实现仍命中 25/33（75.76%），与直接父提交相比没有问题增加或减少命中；此前固定对照 `26b92373` 为 23/33（69.70%）。所有返回均逐条核对正文、原文件摘要和物理页码。检索测量耗时 47.38 秒、内存峰值 167,304 KiB。两项检索测量均未与编译或其他基准竞争资源。该联合范围与平台未公开的 1,151 页、24 个问题组合不同；平台未公开的非标注页冻结词项数仍不能核对，尚不宣称 PDF 题正式 50 分。记录为 `.competition/results/powercontext-pdf-final-quality-validation.json`、`powercontext-pdf-book-value-linux.log`、`financebench-book-value-linux.log`、`financebench-mixed-book-value-linux.log`，以及本地 `.competition/financebench-quality/` 中的解析与检索详细测量。

单 SQL 混合检索已用 173 篇真实 SciFact 摘要、20 个原始问题和 384 维单位向量，完成三个独立 scope 共 60 次原生对照。向量由文本特征生成，相关性标签在全部产品调用结束后才加载计分。每对查询复用同一数据库、scope、当前 head、embedding 和已入库身份；两条路径各预热一次，交替先运行候选或对照。冻结双查询路径的两个索引源码文件与 `9db1753434162c0aa7c9f1189a1722e617880a52` 逐字相同；测量仅在外部切换索引策略，产品完整搜索、融合与正文读取流程一致。所有候选查询恰好一条 SQL，对照恰好两条；实际 EXPLAIN 同时包含 `TEXT RETRIEVAL SCAN` 和 `VECTOR INDEX`。所有返回的当前版本、精确正文、scope/head 和通道标识均有效，没有重复身份；逐条公开 citation 回查及重复查询稳定性检查通过。

候选与对照的 60 次完整结果 tuple 均相同，每条 Recall@10 和 MRR@10 都不降低；平均 Recall@10 为 95%，平均 MRR@10 为 92.5%。单核、2 GiB 限制下，候选完整搜索延迟中位数为 36.31 ms，对照为 36.40 ms，二者基本相当，不能据此宣称端到端加速。含入库与额外引用核验的整个测量耗时 28.99 秒、内存峰值 141,644 KiB。这是独立文本特征向量上的结果，不能代替平台隐藏向量的质量验收。输入、逐查询记录、冻结源码摘要和 EXPLAIN 位于本地 `.competition/hybrid-quality/`，汇总为 `.competition/results/powercontext-hybrid-quality-validation.json`，日志为 `hybrid-quality-expanded.log`。


File SQL 已实现 CSV、JSONL、Parquet 的原生读取，SQL 命名选项、别名和动态类型，包含 DESCRIBE、文件发现、批量取数、重扫、聚合及 JOIN。COPY 支持表或查询输出、表头与 CSV 选项，目标已存在时拒绝覆盖；空查询也会发布表头，查询失败会清理临时文件。Linux 原版 `file_sql_basic` 和新增 `file_sql_queries` 两项 mysqltest 均通过；原版用例及原始数据保持原样。结果保存于 `.competition/results/seekdb-file-sql-final.json`。

格式与编码层的 8 项 Rust 测试、格式检查和 Clippy 全目标检查通过，日志位于 `.competition/results/file-scan-quality.log`。原生 SQL 另外通过 10 项权限专项和 11 项大文件及失败路径专项：覆盖旧连接实时遵守全局目录规则、NULL/空规则、目录前缀与符号链接、FILE 权限及启用角色、跨 2,005 行批次、分组、跨格式 JOIN、LEFT JOIN 补 NULL、全 NULL 列，以及 300 行合计 19,800,000 bytes 的字段处理。中途修改输入文件会中止 COPY；并发抢先创建的目标保留原内容，临时文件清理验证通过。两份记录为 `.competition/results/seekdb-file-sql-policy.json`、`.competition/results/seekdb-file-sql-streaming.json`。这些是自建数据的独立测试，不替代隐藏测试或平台成绩。

File SQL 该轮测试实例为 `/root/oceanbase2026/instances/core-files`，验证二进制另存为 `/root/oceanbase2026/seekdb-file-sql-empty`。整合 File SQL 后，既有 12 项 AI mysqltest 再次全部通过，记录为 `.competition/results/seekdb-ai-after-file-sql.json`，模型响应仍来自本地确定性 HTTP fixture。新增系统变量的生成器已验证重复生成一致，原有 719 个变量的赋值保持一致；增量构建会跟踪语法源文件并重新生成解析器。文件发现和类型边界仍继续做专项复核。

文件发现进一步改为逐目录扫描，限制保留 10,000 个路径、检查 100,000 个目录项及路径，并周期性检查查询取消状态，避免系统 glob 在检查上限前先分配所有匹配项。新增 9 项独立检查全部通过，包括目录通配、合法目录链接、隐藏文件、无匹配、10,001 个匹配文件及 100,001 个不匹配文件的超限路径。连续 30 次超限后，实例文件句柄数保持 25；同轮两项 File SQL 综合用例和 10 项权限专项再次通过。记录为 `.competition/results/seekdb-file-sql-glob.json`、`.competition/results/seekdb-file-sql-bounded.json` 和上述权限记录。该轮实例使用 `/root/oceanbase2026/seekdb-file-sql-bounded` 的副本，其 SHA-256 为 `7b2a339bcab4a88b7bfae6fea6943af6eb9bfad55d1b414b8c6791310bf4e45c`。

AI COLUMN 的 DDL 阶段已完成原生实现，定义作为列元数据持久保存，提供目录视图 `__all_ai_column`，并建立后续使用的持久化任务表。每个 AI 列具有三个独立隐藏列；修改定义会复用这些列并更新定义版本，删除列时清理活动元数据。原版 `ai_column_ddl` 和新增 `ai_column_ddl_changes` 两项 mysqltest 通过；27 项独立检查覆盖普通字段前向引用、普通列转 AI 列、批量添加、修改参数、删除依赖保护，以及非法选项、嵌套 AI、AI 列依赖和维度不一致等拒绝路径。实际停止并重启实例后，修改过的定义、SHOW CREATE 和隐藏列归属逐项一致。记录为 `.competition/results/seekdb-ai-column-ddl.json`、`seekdb-ai-column-ddl-independent.json` 和 `seekdb-ai-column-restart.json`。

DDL 阶段原生实例为 `/root/oceanbase2026/instances/core-ai-column`，管理路径为 `/root/oceanbase2026/seekdb-ai-column-ddl`，其验证副本 `seekdb-ai-column-ddl-modify` 的 SHA-256 为 `ece2d0ec169304d3df3422a1452fa062bbb553dda45708d3572c7533588f9b48`。此前的 `core-files` 保留并停机。该版本的既有 12 项 AI 函数 mysqltest 再次全部通过，记录为 `.competition/results/seekdb-ai-after-ai-column-ddl.json`；AI 响应仍为本地协议 fixture。该 DDL 阶段尚未验证后台模型调用、Strict Read、手动重试和向量索引维护；后续记录包含这些功能的实际验证。

AI COLUMN 的业务写入现已在同一事务内清空受影响结果、更新隐藏状态并写入持久化任务；同一行受影响的列共享请求标识，无关字段更新保留结果与任务。12 项独立 SQL 检查全部通过，覆盖提交、回滚、主键变更、批量插入与删除、显式结果赋值拒绝和隐式列清单。新增 `ai_column_transactions` 回归进一步验证语句失败、保存点及整事务回滚，以及 REPLACE、冲突更新、INSERT SELECT、联表更新、二进制和微秒时间戳主键。不依赖在线模型；两项既有 DDL 和 12 项 AI 函数 mysqltest 同轮通过。记录为 `.competition/results/seekdb-ai-column-t1-independent.json`、`seekdb-ai-column-t1-regression.json` 和 `seekdb-ai-after-ai-column-t1.json`。

AI COLUMN 后台计算已接入原生服务生命周期，支持持久任务认领、实际 Endpoint 并发限制、重试、过期任务隔离和同一行结果的事务发布。新增或修改 AI 列会对已有数据分批计算，使用主键游标避免反复扫描已处理的行。输出通过原生类型和精度检查；唯一索引或检查约束拒绝某列时，该列结束为失败，其余有效列仍在同一事务中发布。原版 DDL、T1、Worker、向量索引用例及新增 DDL、事务、回填回归，共 7 项 AI COLUMN mysqltest 全部通过；同轮 12 项 AI 函数回归全部通过。新增回填用例覆盖 65 行、同组原子可见、修改定义重算、AI_PROMPT 与普通文本区分、请求配置保护、约束失败收尾及无显式主键的重复行。

15 项独立后台检查全部通过，包括两个共享 URL 的 Endpoint 实测并发峰值分别为 2 和 1、非法整数/日期/JSON/超长文本/数值溢出收尾，以及二进制、UTF-8 和时间戳主键的写入及回填。另一次真实测试实例中断验证在 HTTP 请求进行中发送 SIGKILL，重启后 7.958 秒完成租约恢复和任务清理；这些是自建协议 fixture 的结果，不是平台提测。记录为 `.competition/results/seekdb-ai-column-worker.json`、`seekdb-ai-column-worker-independent.json`、`seekdb-ai-column-worker-restart.json` 和 `seekdb-ai-after-ai-column-worker.json`。两项 File SQL 回归同轮通过，记录为 `.competition/results/seekdb-file-after-ai-column-worker.json`。该 Worker 阶段验证副本为 `/root/oceanbase2026/seekdb-ai-column-worker-constraints2`，SHA-256 为 `8e45e4ce0d6c09ea30b06b0e6bf82fbf418d7b651abd6f058fab7cc2d05814a3`；管理路径继续为 `seekdb-ai-column-ddl`。该阶段尚未实现 Strict Read 和手动 RETRY；后续分别补齐并验证。

Strict Read 已增加 `ai_column_strict_read` 开关，默认关闭。开启后，在语句使用的事务快照中，对实际引用的 AI 列做全列就绪检查；待处理、失败或定义版本过期时返回 11120。普通列、独立就绪列、空表和已就绪的 NULL 结果可读；缓存计划每次执行都遵守当前开关。新增回归通过 13 项输出断言和 9 项预期错误检查，包含视图、投影裁剪、别名、子查询、拒绝写入、缓存 PREPARE 切换及空向量。另有 16 项独立检查通过，覆盖自己的未提交写入、其他会话隔离、可重复读快照、全局设置继承和定义版本过期。实际重启后，全局设置和 READY NULL / FAILED NULL 的区别均恢复，4 项检查通过。

严格读取版本在全新测试目录 `/root/oceanbase2026/instances/core-ai-column-strict` 上验证，使用 `/root/oceanbase2026/seekdb-ai-column-strict-initial`，SHA-256 为 `5ff6bda6a7cca37ac1f8c1584eea16efdd6e471b21cb5787e0c2a04324ad275b`。旧目录直接启动新增开关版本时因缺少变量记录退出，现保留停机，原管理路径已恢复为上一阶段的 Worker 验证副本。完整回归为 8 项 AI COLUMN、12 项 AI 函数、2 项 File SQL 全通过；记录为 `.competition/results/seekdb-ai-column-strict.json`、`seekdb-ai-column-strict-independent.json`、`seekdb-ai-column-strict-restart.json`、`seekdb-ai-after-ai-column-strict.json` 和 `seekdb-file-after-ai-column-strict.json`。原有 720 项系统变量定义逐项保持一致，新增开关使用新 ID。

手动 `ALTER TABLE … RETRY AI COLUMN … [WHERE …]` 已补齐，只重试指定列和命中行，支持 READY、FAILED、PENDING 状态及普通业务条件。未加引号的 `status` 是计算状态，反引号或表名限定的同名字段仍表示业务字段。操作通过当前用户身份执行原生 UPDATE，结果失效和任务登记同事务提交；运行时授权检查防止内部缓存计划被普通写入复用。原版重试与严格读取用例、新增组合主键和事务边界用例均通过，全部 10 项 AI COLUMN 回归记录为 `.competition/results/seekdb-ai-column-retry.json`。

17 项独立检查进一步验证二进制组合主键、隐藏主键重复行、条件错误时回滚、关闭自动提交时的 DDL 提交边界、普通账号重试及拒绝越权子查询，以及 VSAG 索引失效与重新发布。另复现并修复修改定义后的旧结果窗口：同一组 256 行数据，修复前操作返回后有 4 行仍返回旧结果，修复后为 0；修改操作会同步失效尚未登记新版本的行，后台继续计算。记录为 `.competition/results/seekdb-ai-column-retry-independent.json`、`seekdb-ai-column-ddl-visibility-before.json` 和 `seekdb-ai-column-ddl-visibility-after.json`。这组行为验证使用副本 `seekdb-ai-column-retry-behavior-validated`，SHA-256 为 `178694a048bec4058f8413cb65471acbfdac5fae09490b4101a6df8930808c5a`；随后只修正两处非法重试语句的错误文字。最终回归副本为 `/root/oceanbase2026/seekdb-ai-column-retry-final`，SHA-256 为 `dea2203fd8b9b1582b62298c056b3713765bf6bfec45c55a306479d65c9356d6`，该阶段运行于上述 strict 测试目录；当前实例已更新为后续 TOKENIZE 修复版本，管理路径保持不变。尚不据此宣称平台 50 分。 最终副本的同轮 12 项 AI 函数与 2 项 File SQL 回归亦全部通过，记录为 `.competition/results/seekdb-ai-after-ai-column-retry.json` 和 `seekdb-file-after-ai-column-retry.json`。

## TOKENIZE 大字段边界修复

提交 `de8d77e43866534a6f7fb3b060997ab81081b35d` 修复 TEXT 内容直接分词时报错或丢词，以及以 TEXT 传入分词器名称时读取内部定位信息的问题。TEXT 内容和分词器名称通过现有 LOB 读取接口取得实际文本；后续提交 `148d0c737ceffb5c3d3c4ced21800ee0ce7199f9` 为普通字符串保留直接读取路径；需要物化的文本使用贯穿本次表达式求值的参数分配器，保留原有空值及名称去空格行为。词频表按输入缓冲区长度与实际内容长度的较大者估算容量，使行内文本保持已有输出顺序，行外长文本获得与实际内容相应的容量。

原生对照覆盖 TINYTEXT、TEXT、MEDIUMTEXT、LONGTEXT，行内阈值 4096 与强制行外阈值 0，四种分词器、两种输出格式、普通与大字段分词器参数，以及空串、NULL、中文、emoji 和 13,200 字节长文本。896 组中，修改前 520 组报错、156 组词项内容不符、220 组符合参考；最终版本 896 组全部匹配，四项独立词数/词频断言全部通过。另用 8 个客户端完成 128 次并发核对。长文本明确返回 alpha、beta 各 1,200 次，总词数 2,400。

原版 basic_dml、cn_word、parser_properties、simple_query、tokenize_function 五项用例在对照与最终版本上均通过；新增 tokenize_lob 回归在对照版本失败、最终版本通过，原版测试和期望文件保持原样。最终 Linux RelWithDebInfo 构建耗时 13.87 秒，二进制 `/root/oceanbase2026/seekdb-tokenize-lob-fast` 的 SHA-256 为 `99c2b91055fa6059efecda0f0ce0fdad36d969c861eeee08cf53784754dc1cc0`。105 个已变更构建输入与服务器逐文件一致，清单 SHA-256 为 `5ec1238cc739f59d37c1839b1c9cf4fd3f1da20fa9a81452892bb4ac482ed818`。本项记作内核正确性修复，基础实践的指定 Bug 材料仍为 #1477。

初版直接解码后曾有一项原版用例仅词项排列不同，最终容量估算已保留既有顺序；独立对照中无类型 NULL 的参考字符集差异也已修正并重新核对。对照旧版产生的非法 UTF-8 错误输出以原始字节捕获，未修改原生用例来绕过错误。早期失败、最终成功和构建产物记录命名修正均保存在本地 `.competition/tokenize-lob/`，最终功能记录为 `results/tokenize-lob-fast-validation.json`。

修复的首轮同机性能对照曾观测普通字符串分词耗时上升 6.76% 和 9.38%，因此增加 VARCHAR 直接读取路径，并对最终版本重新进行完整 A/B/A 对照。原版默认负载、20,000 行、3,000 次分词、每项查询 200 轮、3 组样本和 30 轮预热均保持原样，使用 CPUQuota 400%、MemoryMax 5G 的同一隔离实例；未与编译或其他质量测量重叠。中文分词对照均值和最终版本均为 0.0751 ms；英文从 0.03535 ms 到 0.0348 ms（观测下降 1.56%）。四类查询命中仍为 8,001、11,000、7,332、20。查询计时变化作为原始测量保留，本项交付以修复大字段读取并维持常规分词性能为依据。记录为 `results/tokenize-lob-fast-performance.json`，完整保留首轮负向结果及两轮对照日志。

## TOKENIZE 可选配置空值与运行实例

最小复现 `SELECT TOKENIZE('alpha beta','space',NULL)` 在上一版原生实例上返回 2013（查询期间连接丢失），随后普通查询返回 2002（无法连接），隔离实例进程退出；探测进程已确认 core 文件上限为 0。[官方参数说明](https://www.oceanbase.com/docs/common-oceanbase-database-ai-1000000006781801)将第三个配置参数定义为可选 JSON 数组。本次修复检查 JSON 读取辅助函数返回的 SQL NULL 标志，将 SQL NULL 配置按省略参数处理；JSON 文档中的 `null`、非数组和非法数组元素仍按原规则拒绝。

提交 `3c2738049ea434300517356249ee4bbe897861bb` 新增一对原生回归文件，覆盖字面量、变量、表达式、TEXT/JSON 字段中的 SQL NULL，以及预备语句在 NULL、all 输出配置和再次 NULL 之间切换。192 组对照（四种分词器、六种输入、八种空值来源）、六组错误配置及其后的连接可用性检查、六项独立词数断言和 128 次并发核对全部通过。该轮进程身份在验证前后相同，未以重启恢复来代替持续可用。

同一二进制的 31 项原生用例全部通过：AI 函数 12、AI 列 10、File SQL 2、FTS 7。File SQL 使用现有本机 SSH 样例复制方式，临时授权已清理；模型 HTTP 响应来自本地确定性 fixture。原版测试、期望文件、负载和评分文件保持原样。最终 RelWithDebInfo 构建耗时 14.41 秒，105 个构建输入与服务器逐文件一致；源码清单 SHA-256 为 `2a9243c7f99d279a599cf08eb9ae0317b7c19f6165008b7412c3063f2e41cd16`。

验证快照为 `/root/oceanbase2026/seekdb-tokenize-null`，SHA-256 为 `970e2a68101a5053602fb95b130d3fce945dcd4e545f544bbc95f804f719042d`。现已更新原测试主实例 `/root/oceanbase2026/instances/core-ai-column-strict`，管理路径保持 `seekdb-ai-column-strict-initial`，SQL 端口 2881。更新前无其他活动查询，待处理任务为 0；旧版 `dea2203f…` 快照保留可回退。更新用时 2.38 秒，89 个 AI 列的表/列 ID 与定义版本摘要、严格读取设置和空任务队列均保持一致，部署标识文件中的 ID 保持 `8df70a0e-f2ad-8202-8b3b-ad658ab165d9`。实际运行实例已通过空配置和 #1477 最小查询检查。

原始构建、对照、31 项原生测试、源码核对、运行实例更新与连接关闭后的独立检查，保存在本地 `.competition/tokenize-null/`。正式提测与公开仍按用户要求暂缓。

## 基础实践指定 Bug 修复

已按官方赛题标签要求准备 [#1477](https://github.com/oceanbase/seekdb/issues/1477)：IFNULL 遇到标量子查询及偶数层 NOT / LIKE 时会报 4016。两处 NOT 简化替换现使用既有辅助函数保留原表达式类型，避免父表达式重新推导时重复添加隐式转换。独立提交仅含一个 C++ 文件和一对 subquery 原生回归文件。新增用例修复前报错、修复后通过，原版 `datatype.bool` 与 `subquery.subquery_sj_firstmatch` 同轮前后均通过。576 组独立查询在修复前有 54 组报错，修复后返回值及线协议列类型码全部与 SQL 三值语义、`NO_REWRITE` 对照一致。Linux RelWithDebInfo 整合构建成功，103 个已变更构建输入与服务器逐文件一致；整合版本的 AI 函数 12、AI 列 10、File SQL 2 和 FTS 5 项用例共 29 项均通过。首次 File SQL 因样例复制所需的本机连接配置未就绪而失败，补齐配置后两项通过，原失败记录保留。该记录不是完整 master 构建或官方 CI。详细复现、提交与证据范围见 [Bug 修复材料](issue-1477-fix.md)。

## 后续工作

1. 保留 seekdb 已实现代码、验证快照及指定 Bug #1477 的独立私有分支。公开 Fork、官方 Bug PR 与本队验证行按用户后续指示处理。
2. 保留 PowerContext 最新 95 项原版及 9 项 Pi 回归验证和独立质量、成本记录；正式质量序列与冻结非标注字数仍待平台核验。
3. AI 生态题暂缓，保留已有私有材料。
4. 正式提测暂不执行。保留可复现交付物和测试记录，后续按用户指示安排。
