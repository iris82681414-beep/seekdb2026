# 生态赛题交付约束

七题各 20 分。11 月 5 日正式核验：高德、阶跃星辰各取得 20 分，魔搭保留此前 20 分；Dify 首轮正式验收 0 分；增强后的 18 节点工作流已完成 13 组真实引擎运行及重启持久化核验，7352eeae… 已于 18:16 提测，最近观察仍待执行。以下记录各项材料与剩余条件。

| 题目 | 必须完成的实际工作 | 主要交付物与资源 |
| --- | --- | --- |
| [AMD](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb) | 从 V5 冻结提交继续优化 GPU CAGRA 构建，持久化 GPU 快照并使用固定 hipVS 搜索；lib=vsag CPU 路径须可用 | 正式同轮配对评测为正确性 4 分、构建 14 分、查询 2 分；满分分别要求综合构建加速 3 倍、查询加速 1.5 倍。查询是 4,096 次单行 SQL。两组数据集的 Recall 还须不低于配对基线减 0.005，不能只满足绝对门槛 |
| [MiniMax](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qlqnoryzrs7gtqbw) | 工程上下文模块从数据库检索规格、代码和测试；MiniMax 实际修改代码；PowerContext 保存状态并在重启后复用；规格更新后重新导入生效 | `minimax/` 源码、修改前后快照、相同目标测试由失败到通过的记录、回归结果、数据库及模型使用证据。一页技术说明。六项必做全部通过才得分，不能用环境失败代替修改前的功能失败 |
| [Qoder](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/uygu3273xzlrcmrm) | 主要开发工作真实使用 Qoder，项目核心接入 seekdb 或 PowerContext，通过 Qoder Sites 发布公开项目网站 | `qoder/` 项目、可复现样例、关键 Qoder 开发截图/录屏、个人账号 UID 和公开站点。只用 Qoder 发布其他工具完成的项目不符合要求；实际用户效果可以加分，必须可核对。细则权重待公布 |
| [阶跃星辰](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/kw9wfx4bu9pyvat9) | 浏览器本地完成文档和查询 embedding；INT8/FP16；WebGPU 优先并自动降级 WASM；经 seekdb-js 存储，混合检索在 seekdb 内完成 | `stepfun/` 可运行 Web 应用、源码、数据和一页技术说明。提供模型体积、量化前后质量、下载/加载/推理/写入刷新/检索分段计时；分别实测两条后端路径。不得调用云推理或回放预计算向量 |
| [Dify](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lhfslihtnnb8yqh6) | Dify 承担核心流程，制作可体验的趣味应用；seekdb 或 PowerContext 对功能或数据库部署实际起作用 | `dify/` DSL、可复现步骤、初始化材料、完整体验样例和真实运行证据；明确 Dify、插件、模型与数据版本。基础可运行性之外还评价创意、互动和工程完成度，权重待公布 |
| [高德地图](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qp60miwsbk1mcgod) | 单轮周边推荐：真实 POI、同一起点的独立步行路线、原始时长硬筛选、最多三项和可核对理由 | 仅上传一个 SKILL.md；平台配置模型、工具与 API Key，免交额外应用。规则与 AI 评审权重为数据 40%、需求 30%、合理性 20%、表达 10%。当前 `gaode/SKILL.md` 已完成，格式校验通过，已通过正式评测，20/20 |
| [魔搭社区](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/bt7u0qwwz84gt48g) | 将自己编写的 Skill 成功上传到魔搭，内容可查看，再提供准确的作品链接或编号 | 不以个人主页或账号 ID 代替作品地址。11 月 5 日核实团队在 10 月 30 日已执行成功、取得 20/20；本轮未重新公开上传或提测 |

生态代码基于官方 `2026-competition-ecosystem`，基线 `d647275`，本地工作目录 `.competition/ecosystem-work`。高德保留单文件 Skill。阶跃星辰通过前端构建和真实 seekdb 原生混合检索链路验证，覆盖增量写入、刷新、重复导入及原文回读。用官方 FP32、INT8 权重在原生 CPU 后端验证 12 篇短文与 6 条查询，两个版本的 6 条预期原文均排首位。11 月 5 日新增本地权重和推理运行时配置，真实浏览器已完成 WASM INT8 的写入和检索。MiniMax 的规格检索和 PowerContext 状态重读通过独立进程复测，重导入后返回新规格；模型实际修改及修改后回归仍待执行。AMD 已检出官方 V5 基线 `634888d9…`，满分的构建分要求达到 3 倍加速，需在 Radeon W7900D 和 ROCm 7.2.4 环境实测。

Dify 候选为《末班月亮档案室》。本地已交付 DSL、六份原创案卷和 seekdb 检索服务。五条实际 HTTP 交互均以真实数据库结果驱动 DSL 中的代码节点，覆盖成功和失败的指认、线索关联及无匹配提示。Dify 1.10.1 官方容器已启动，DSL 更新为 0.5.0 并无警告导入；五条样例经真实 HTTP、代码沙箱与结束节点执行，全部 succeeded，约 0.49–0.63 秒，证据随 `ec018937…` 提交。阶跃星辰另在同一浏览器启用真实 WebGPU FP16，独立集合写入 12 篇并查回相同的预期首条原文。

此前高德、阶跃星辰和 MiniMax 候选已推送到私有 `2026-competition-ecosystem` 分支，远端回读确认提交 `a2d330d10ef07f8225059a1c6bda0a263dd15cac`。11 月 5 日另将最新公开推送至原报名仓库：高德与阶跃的评测固定提交为 `f2c40932…`，两题均为 20/20；Dify 固定提交为 `ec018937…`，正式验收为 0 分。[私有生态分支](https://github.com/wsaitr/seekdb2026-answers/tree/2026-competition-ecosystem)

尚待解决的项目：AMD 绑定地址的只读令牌格式（平台本身提供 GPU 统一评测，无需先购买本地 GPU），MiniMax 的真实模型调用方式，Qoder 个人账号与开发/发布权限。当前环境没有发现已安装的 Qoder CLI，也没有发现对应服务的凭据环境变量；这不等于用户账号不存在。可以先准备不依赖这些资源的代码和复现材料，但不得伪造其使用记录或性能结果。

## 仍需账号持有人完成的操作

11 月 5 日核对了实际页面，以下依赖不影响继续优化全文索引：

| 项目 | 当前证据 | 需要完成的操作 |
| --- | --- | --- |
| MiniMax | 官方 MiniMax Code 已能加载，当前未登录；统一登录页要求确认服务条款 | 账号持有人完成登录并自行确认条款，或在本地进程配置本次比赛可用的模型凭据。已有真实数据库上下文模块和续借样例可继续用于模型修改与回归，凭据不经聊天传递 |
| Qoder | 官方账号入口重定向到登录页，当前无可用登录状态 | 登录个人 Qoder 账号，取得 UID，保留由 Qoder 实际承担主要开发的记录，并通过 Qoder Sites 发布可公开访问的作品 |
| AMD | 当前注册仓库 URL 不带题面强制的只读令牌，提测表单锁定仓库地址；有推送权限尚不能证明能修改报名绑定 | 由有权修改报名信息的队员或组委会更新读取配置。仓库令牌仅授予指定答题仓库 Contents: Read-only，按题面要求绑定；不要使用已有写入令牌替代 |

MiniMax 登录页涉及条款同意，该步骤由账号持有人操作。Qoder 的认证与 Sites 能力以官方 [认证说明](https://docs.qoder.com/cli/authentication)、[Sites 说明](https://docs.qoder.com/qoder/sites) 为准。Dify 首轮记录保留；新版的编排、进度持久化和完整原始运行证据已提交，等待平台返回结果。
