# 生态赛题交付约束

七题各 20 分。以下要求来自 2026-10-26 当天题面；开发、真实性证据和平台得分分别记录。按用户最新要求，AI 生态题整体暂缓。当前不执行正式提测，也不公开上传 Skill。以下仅保留已核对的要求供后续参考。

| 题目 | 必须完成的实际工作 | 主要交付物与资源 |
| --- | --- | --- |
| [AMD](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/seebnbaoxot1b0tb) | Radeon GPU 和 hipVS 参与 seekdb HNSW 的构建核心；完整构建产物可查询，CPU 路径保留 | seekdb PR、复现方法、至少三轮 CPU/GPU 构建中位数及 Recall@K。计时涵盖传输、同步和格式转换。题面引用的 12.65 倍是批量查询数据，不能当作构建成绩。需实际 AMD GPU，统一目标倍数尚待公布 |
| [MiniMax](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qlqnoryzrs7gtqbw) | 工程上下文模块从数据库检索规格、代码和测试；MiniMax 实际修改代码；PowerContext 保存状态并在重启后复用；规格更新后重新导入生效 | `minimax/` 源码、修改前后快照、相同目标测试由失败到通过的记录、回归结果、数据库及模型使用证据。一页技术说明。六项必做全部通过才得分，不能用环境失败代替修改前的功能失败 |
| [Qoder](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/uygu3273xzlrcmrm) | 主要开发工作真实使用 Qoder，项目核心接入 seekdb 或 PowerContext，通过 Qoder Sites 发布公开项目网站 | `qoder/` 项目、可复现样例、关键 Qoder 开发截图/录屏、个人账号 UID 和公开站点。只用 Qoder 发布其他工具完成的项目不符合要求；实际用户效果可以加分，必须可核对。细则权重待公布 |
| [阶跃星辰](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/kw9wfx4bu9pyvat9) | 浏览器本地完成文档和查询 embedding；INT8/FP16；WebGPU 优先并自动降级 WASM；经 seekdb-js 存储，混合检索在 seekdb 内完成 | `stepfun/` 可运行 Web 应用、源码、数据和一页技术说明。提供模型体积、量化前后质量、下载/加载/推理/写入刷新/检索分段计时；分别实测两条后端路径。不得调用云推理或回放预计算向量 |
| [Dify](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/lhfslihtnnb8yqh6) | Dify 承担核心流程，制作可体验的趣味应用；seekdb 或 PowerContext 对功能或数据库部署实际起作用 | `dify/` DSL、可复现步骤、初始化材料、完整体验样例和真实运行证据；明确 Dify、插件、模型与数据版本。基础可运行性之外还评价创意、互动和工程完成度，权重待公布 |
| [高德地图](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/qp60miwsbk1mcgod) | 单轮周边推荐：真实 POI、同一起点的独立步行路线、原始时长硬筛选、最多三项和可核对理由 | 仅上传一个 SKILL.md；平台配置模型、工具与 API Key，免交额外应用。规则与 AI 评审权重为数据 40%、需求 30%、合理性 20%、表达 10%。当前 `gaode/SKILL.md` 已完成，格式校验通过，仍待实际评测 |
| [魔搭社区](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/bt7u0qwwz84gt48g) | 将自己编写的 Skill 成功上传到魔搭，内容可查看，再提供准确的作品链接或编号 | 不以个人主页或账号 ID 代替作品地址。可复用已完成的原创 Skill，用户要求暂不公开，保持本地和私有仓库保存；当前不提测 |

生态仓库基于官方 `2026-competition-ecosystem`，基线 `d647275`，本地位于 `.competition/ecosystem`。高德 Skill 已提交到用户私有仓库同名分支。所有配置示例只使用占位值，不包含真实密码或令牌。

尚待外部资源的项目：AMD GPU 的实际可用入口，MiniMax 的真实模型调用方式，Qoder 个人账号与开发/发布权限。当前环境没有发现已安装的 Qoder CLI，也没有发现对应服务的凭据环境变量；这不等于用户账号不存在。可以先准备不依赖这些资源的代码和复现材料，但不得伪造其使用记录或性能结果。
