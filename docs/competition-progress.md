# 2026 OceanBase 初赛实施进度

大赛总分 620。按用户最新要求，当前只推进 seekdb 内核与 PowerContext；AI 生态题暂缓，正式提测和公开上传暂缓。平台已确认得分为 0；公开测试和独立测量不等于正式成绩。

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
| AI_EMBED 单行、AI_COMPLETE | 20 | 请求/响应核心实现通过长文本、空配置、JSON 配置、维度及 16 组异常响应的原生回归，待正式完整验收 |
| Ollama Provider | 10 | 原生免 Key、可选认证、生成、参数和向量路径回归通过；同轮七项 mysqltest 全通过 |
| AI_RERANK、跨行批量 AI_EMBED | 20 | 待实现，官方综合用例仍需全部通过 |
| 文件即数据库 | 50 | 首轮基线在本机 SSH 文件分发步骤失败，须补齐测试环境后定位产品缺口 |
| AI 列 | 50 | 原版用例在 AI COLUMN 语法处失败，待实现 |
| 全文索引性能 | 50 | 已准备独立实例和冻结基线的原版测量，待完成基线并优化 |
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

seekdb 官方基线、AI_PROMPT 及后续请求/响应修改均已在服务器构建成功。新增请求验证覆盖 50,000 字符的真实 LOB 读入；16 组异常响应通过 loopback HTTP 返回，SQL 均拒绝异常输入且实例继续正常响应。基线可执行文件另存于 `/root/oceanbase2026/seekdb-baseline`。实例部署标识为 `becf1a63-c20a-8684-a088-a2f704ce1127`，来自实例 `run/telemetry.json` 的 `content.id`，`sent=true`；平台同步和核验仍待正式操作。

## 后续工作

1. 完成其余 AI 函数、File SQL、AI COLUMN 和 FTS 性能任务，使用原版测试验证。
2. 继续核验 PowerContext 的完整质量及成本门槛，不能把接口用例通过视为满分。
3. AI 生态题暂缓，保留已有私有材料。
4. 正式提测暂不执行。保留可复现交付物和测试记录，后续按用户指示安排。
