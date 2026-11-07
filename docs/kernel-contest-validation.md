# 内核赛题增量验证记录

## 本次基线

- 仓库：`iris82681414-beep/seekdb2026`
- 来源分支：`feat/ai-function-enhancements`
- 基线提交：`9d153454ff5b5c0e471134e5448d22b7ec6d8afc`
- 工作分支：`feat/kernel-contest-validation`
- 验证环境：macOS / arm64，AppleClang 21，macOS SDK 27.2，Rust 1.98.1。

该基线已有 AI 函数增强、文件 SQL、异步 AI COLUMN 和全文词频表锁优化。本次保留这些实现，只进行增量修复。历史文档中的其他环境通过记录不算本次实测结果。

## 已实际执行的验证

| 项目 | 结果 | 范围 |
| --- | --- | --- |
| `build.sh release --init` | 通过，退出码 0 | SDK 初始化、CMake 配置和生成；**不是生产二进制编译** |
| `cargo test -p file_scan` | 8 通过、0 失败，退出码 0 | 修改前基线，涵盖 CSV、JSONL、Parquet、多 RowGroup、投影、重扫、文件身份和 CSV 导出回读 |
| 修改文件 `rustfmt --check` | 通过，退出码 0 | `jsonl.rs`、`tests.rs`；只验证格式 |
| `git diff --check` | 通过，退出码 0 | 只验证 diff 空白格式 |

基线 Rust 命令从仓库根目录执行：

```sh
PATH=/opt/homebrew/opt/rustup/bin:$PATH cargo test --manifest-path rust/Cargo.toml -p file_scan
```

## 当前增量与验收状态

### AI COLUMN：关闭客户端隐藏重试

任务队列已有 `max_retries` 和重试状态，但 completion、embedding helper 原先内部创建默认客户端。默认 scalar 客户端在持续 429/503 时最多发送四次请求，导致一次任务尝试不等于一次 HTTP 请求。

增量将重试策略显式从 `ObAIFuncModel` 传到所有内部 `ObAIFuncClient`。普通 AI 函数保持默认策略，AI COLUMN worker 关闭客户端重试，由队列决定后续 dispatch。批量客户端在关闭重试时仍发送初始请求，避免零次循环却返回成功。

在既有 `unittest/sql/engine/expr/test_ai_http_client.cpp` 添加本地回环 Mock 的请求计数覆盖：

- 普通 scalar 调用保留四次请求；
- 关闭重试的批量调用发送每个初始请求并返回真实 HTTP 错误；
- completion / embedding helper 对 429、503 每次 dispatch 只发送一次请求。

**这些新增 C++ 测试尚未编译或执行。** 它们覆盖真实 model→client 网络路径，但不能替代 worker 队列和数据库端到端测试。本增量也未修复或证明 crash recovery 的持久化尝试预留，不宣称 exactly-once 调用或崩溃恢复后的严格请求次数上限。

### 文件 SQL：拒绝包含 NUL 的 JSONL 列名

CSV 表头和 Parquet schema 已拒绝 NUL，JSONL key 原先只检查空字符串和长度，schema inference 的命名路径未补充 NUL 校验。增量在 JSONL 解码边界拒绝该列名，沿用包含行号的错误处理。

在既有 `rust/file-scan/src/tests.rs` 添加首行和后续行出现 `\u0000` key 的回归，要求 schema 发布前失败、错误包含行号、不得输出结果。

**该新增 Rust 回归尚未执行，8 项通过只属于修改前基线。**

## 本次交付范围

本次完整 CMake 编译和新增 Rust 回归执行此前被执行权限检查拦截，命令未运行。用户随后明确要求「不要编译，直接 push」，因此本次直接提交增量源码、回归测试及本记录，不继续构建或执行测试。

没有候选生产二进制，没有候选 SQL 结果，也没有候选全文性能数据。不使用已有二进制冒充本次源码验证，不提交未验证的比赛成绩。本次推送不代表四项赛题全部完成。

后续另行验证时可运行：

```sh
PATH=/opt/homebrew/opt/rustup/bin:$PATH cargo test --manifest-path rust/Cargo.toml -p file_scan
PATH=/opt/homebrew/opt/rustup/bin:$PATH cmake --build build_release --target seekdb -j4
```

C++ AI 单测的已有 Bazel 目标为 `//unittest/sql/engine/expr:test_ai_functions`，标记 Linux-only，需要受授权的独立 Linux 构建环境。不得为了绕过本地执行拒绝转交另一工具或主机执行。

## Parquet 资源边界调查

对固定 `parquet = 57.1.0` 的源码检查发现：`ReaderProperties` 没有读取侧的 footer 集合计数、解压页大小或 dictionary entry 限制。元数据 parser 在读取集合元素前按声明数量分配 capacity；page 解码按 header 的 uncompressed size 分配，dictionary decoder 另按声明 entry 数量扩容。部分已启用 codec 的输出还能在最终长度核验前继续增长。

因此仅限制 footer 文件字节、RowGroup 字节或 iterator batch size 不能建立恶意输入的内存上限，读取完成后检查 `Page` 也太晚。当前未升级、替换或复制 decoder，也未用关闭正常 codec 支持伪装完成。完整补缺还需要读取前的有界结构校验、解压输出约束，以及防止校验与解码之间文件变化的同一输入保证；这部分保持未完成。

## 尚未完成的验收

- 候选 Rust 回归、C++ 编译、AI 单测和数据库 SQL 端到端验证。
- Parquet footer 结构计数、page/dictionary 解码预分配及总体内存预算；目前的 footer/RowGroup/column 限制和 batch size 不足以证明这些分配有界。
- AI COLUMN 崩溃恢复、尝试持久化、分组重叠、DDL/backfill、严格读取和向量索引事务验证。
- 六项 AI 函数本次完整 SQL 重跑。
- 原始全文基线、性能瓶颈 profile、候选优化的运行验证及同机 baseline→candidate→baseline 性能验证。

## 全文索引第二轮增量（2026-10-31）

### 平台基准记录

用户提供的平台记录：`feat/kernel-contest-validation` / `faecffc7d819fafd819fa5ba790d6cc6a77b49fd`，2026-10-31 00:18 执行成功，**得分 7.381 / 50**。这不是耗时，也不是已核实的提升百分比。当前没有九项耗时明细、完整结果校验报告或 profile，无法据此判定哪类成本占主导。

### 源码开销依据与修改

1. **词频聚合：两次查找改为一次，并避免重复词替换节点的分配。** `ObAddWord::groupby_word` 原先先 `get_refactored`，再 `set_refactored(..., overwrite=1)`；底层分别哈希及遍历 bucket，覆盖路径另分配替换节点。现在复用 `ObHashMap::set_or_update`，原地更新已有 pair。首次插入仍计 1，后续加 `word_freq`；回调同步替换 key，保留 collation 相等但字节不同的最后词项。非分组重复错误、过滤、doc length 和字符集比较保持原逻辑。
2. **BENG：直接规范化到调用方 allocator，移除中间复制。** 原 normalizer 在每轮复用的私有 arena 中调用 `ObCharset::tolower`，BENG 再在自己的输出 allocator 分配并 memcpy。现在 English analyzer 增加显式持久输出模式，BENG 直接选择它的输出 allocator，默认两参 analyzer 调用仍使用 scratch 模式。字符集转换、修剪、token 顺序、频次和 byte-based char length 不变；输出字节仍由调用方持有，不能借用迭代器下一轮即失效的临时缓冲区。

两项作用于已有共享分词链，供 TOKENIZE、索引构建和查询词处理复用；没有修改检索执行算法、词典或分词规则。这些是源码可确认的重复工作，**不是 profile 证明的主瓶颈，也没有实测加速比例**。

### 新回归与构建声明

新增 `unittest/storage/test_fts_token_pipeline.cpp`，目标 `//unittest/storage:test_fts_token_pipeline`。覆盖旧分组算法对照、非 1 初始频次、collation 等价 key/最后代表值、大小写、停用词/长度边界、非分组重复及非法输入、未初始化 map、默认/持久 analyzer 对照、过滤/聚合开关、BENG 输出在后续 token、EOF、parser 销毁及原输入修改后的有效性。包含长 token、全部保留的 UTF8MB4 collation/binary、非 ASCII 与坏字节输入对照，以及两种 allocator 分离和 analyzer 重初始化；Latin1 枚举已无 backing charset，另校验其拒绝行为。

该目标为 Linux-only 静态链接 C++ 单测；生产 translation units 已在原 inventory 注册，不新增生产清单。CMake inventory reader 原先看到 `unittest/storage/BUILD.bazel` 就要求统一测试农场宏，会拒绝此独立 cc_test 包。本轮最小修正为显式 standalone `cc_test` 保持 Bazel-owned，原统一宏数量与字段校验保留。没有为构建脚本新增测试。

### 本轮实际验证与限制

- `git diff --check` 已通过；接口、调用点和 allocator 生命周期已做静态核对。官方基准五文件 SHA-256 再核对一致。
- 尝试运行源码 inventory 生成命令，但执行权限检查在运行前拒绝：需要显式授权执行目标仓库脚本。**该命令未执行，未生成可供核验的本轮 inventory**，没有换工具或主机绕过。
- 用户询问已有 SSH 后，对已授权主机做只读连接检查，返回 `Connection refused`，退出码 255；未进入远程主机，未改变现有部署。不在仓库发布连接地址。
- 按「不要编译，直接 push」要求，**本轮生产源码与新增 C++ 回归均未编译、未执行**。没有候选生产 binary、候选 SQL 结果或新性能报告；IDE 缺失 include 配置的诊断不算编译证据。

推送后仍须平台或之后单独授权环境验证运行正确性与九项性能。不得使用旧 binary 冒充候选，不能只核对命中 COUNT。官方基准会重建 `fts_large_bench`，只能在专用实例执行，不能对保留部署运行。

## 远程基线构建验证（2026-10-31）

用户之后授权独立远程构建和性能测试，因此本节补充后续实际进展，不改变上节源码提交时未编译的历史记录。

- 公开基线 `faecffc7d819fafd819fa5ba790d6cc6a77b49fd` 的源码快照 SHA-256：`78ae9a1d9defa4701a601ad6c7b6885eb4a5bc663d3b23082fc12dd41a78f3b5`。构建完成后逐文件对照快照中的 **9241** 个普通文件，缺失或内容变化 **0**；候选 patch 尚未应用。
- 使用 SDK Clang 17、CMake 3.30.3、Rust 1.98.1，配置 `RelWithDebInfo`、`OB_USE_CCACHE=ON`、`NEED_PARSER_CACHE=OFF`，执行生产目标 `seekdb`，8 个并行任务。配置及生产编译完成，退出码 **0**。
- 基线 binary 已单独保存，大小 **1367145896** 字节，SHA-256：`880e0f6706d608ddab94614f3b25856977d7b882b2a95191755903d9f73e827c`。
- **内嵌 Git 版本信息不可作为来源证据。** 公开快照没有自己的 `.git`，配置时 `git rev-parse` 向上发现外层 notebook 仓库，导致 binary 显示外层仓库的 `main` / `0b2c8f6…`。以上公开快照及逐文件核对证明实际编译源码来源，不能把该内嵌版本号当作基线或候选提交号。
- 五个官方基准文件哈希仍与下表一致。Rust workspace 的 lockfile 不在公开快照中，由本次正常 Cargo 解析生成，SHA-256：`8b6562e0c2c88bb99c4840c8e6a9703946455fa88acb68682935f73bd8a96cf9`；同机候选应保留同一份解析结果，不能把其他环境生成的 lockfile称作原始公开基线。
- 容器不允许 `perf_event_open` 用户态 CPU 采样，返回 `Operation not permitted`；未修改主机安全设置。准备了独立 debugger sampling 和完整结果对照脚本，但**没有执行 profile、SQL 正确性用例或计分基准**。
- 后续远程写入和测试启动被权限检查拦截，未绕过。候选生产 binary、新增七项 C++ 回归、既有十七项 FTS SQL 用例和九项性能实测仍待执行。已知平台分数仍为 **7.381 / 50**，本次生产编译成功不等于优化正确性或性能验证通过。

## 后续候选构建与原生回归（2026-10-31）

用户再次明确授权完整构建和实测后，继续执行候选验证：

- 首次候选 patch 应用受外层 Git 仓库路径影响，命令虽返回成功但源码未变化，产物与基线 SHA-256 相同。该产物已判定无效，不作为候选证据。随后设置 `GIT_CEILING_DIRECTORIES` 阻止发现外层仓库，并在构建前逐文件核对候选及源码标记。
- `c64867f3797f17e91c945bd14eca17a7a2b02d7f` 的生产修改实际编译、链接成功，独立候选 binary SHA-256：`79a1031054ac3a7ac554be4041336271736e8c47680ba1fa6869cf615360bb03`。保留与基线相同的依赖解析及构建配置；官方基准五文件未改。
- 新增回归首次编译发现 Clang 17 的两项错误：range-loop 的多余字符串复制触发 `-Werror`，以及将 `ObDatum::len_` 位域直接转发给 `emplace_back`。分别改为 const 引用和显式 `size_t` 转换；未放宽生产编译选项。
- 使用实际生产 `compile_commands.json` 和链接图编译、静态链接回归 executable 成功；该方法**不等于 Bazel 单测目标已验证**。实际执行七项回归，**6 通过、1 失败**。失败在 `matches_scratch_normalization_and_owns_retained_tokens` 的 Latin1 charset 非空断言。
- 源码核查发现 `ObCharset::get_charset_uninit` 仅保留 UTF8MB4 general/bin/unicode 和 binary；Latin1 枚举仍存在但无 backing charset。因此修正测试为全部四种保留 collation，并明确断言 Latin1 拒绝行为。该最后修正尚未远程重跑：上传及启动命令被权限检查在执行前拒绝，未绕过，不能声称七项通过。
- 2026-11-01 用户重新明确授权上传及远程实测后，上传命令和独立只读连接检查均返回 `Connection refused`，退出码 **255**。修正文件未上传、回归重跑未启动；该连接故障不算测试失败。恢复实测需要当前可连接的授权 SSH 端点，未尝试其他主机或端口。
- 核查测试包装脚本时修正了初始化顺序：从已存在的 `oceanbase` 数据库连接后执行建立 `test` 库的官方 `init_user.sql`，避免连接尚不存在的 `test` 库即失败。官方初始化文件与计分基准均未修改；包装脚本的该修正尚未远程执行。
- 十七项 SQL 用例、九项计分基准、完整结果对照及独立 profile 仍未执行。没有新性能结果或平台得分；已知平台成绩仍为 **7.381 / 50**，目标 50 分未达到。

## 新服务器恢复验证（2026-11-04）

- 用户提供新 SSH 端点并要求继续。新服务器没有旧独立工作目录或 binary，不能复用旧机器的实测结果；保持原 Notebook 仓库不变，在新独立目录重新准备基线。容器配额为 16 CPU、55 GiB 内存，工作卷约 98 GiB。
- 复用此前下载的公开 `faecffc7d819fafd819fa5ba790d6cc6a77b49fd` archive，重新校验 SHA-256，安全解包到新目录；逐文件对照 **9241** 个普通文件，**0** 缺失或变化。
- 用户明确授权安装 RPM 解包和诊断工具以及独立构建、测试。`rpm2cpio`、`cpio`、`gdb`、`iproute2` 安装完成；官方组件校验通过，Rust/Cargo **1.98.1** 安装到独立工具目录。官方 SDK 初始化与基线 CMake 配置已完成，生产 build 已开始（检查时约 16%），尚未完成。已排队仅在 build 退出码 0 且来源及 binary 哈希复核通过后运行新服务器的首轮九项基线测量；不能把排队当作实测完成。
- 初次 Cargo fetch 从登录目录启动，继承 Notebook 的 rsproxy mirror 配置，出现 `CONNECT tunnel failed, response 403`。未改 Notebook 配置或网络安全设置；终止自己启动的失败 fetch 后，从独立 operator 目录重新启动，确认日志为 `Updating crates.io index`，使用官方源。最终依赖解析须在同机 baseline/candidate 间保持一致。
- 自动模式拒绝上传本地候选补丁，要求人工审查执行权限。用户随后明确授权该补丁上传，但自动模式仍在运行前拒绝，要求退出 auto mode 审查提示。补丁限定为九个生产、构建及回归文件，不含仓库历史、文档、连接地址或 binary，SHA-256 为 `7607f1d578ae7c1dff5c7bbbb1c9c4be9bd363f2d145700b2c4542c7bc9e8fe9`；该补丁**尚未上传或应用**，未绕过拒绝。
- 正常生产 build 正在编译 Rust crates，已解包约 139 个 registry 包；移除自己启动的冗余 fetch，避免争抢 Cargo package-cache 锁。新服务器生成的 workspace lockfile SHA-256 为 `b2379a18b9cdfd9608df45f82f4a2ff5833514eacbc238a35ee49bec0ffea57d`，后续候选保持相同文件，不混用旧服务器解析。
- 基准启动包装显式设置 `_enable_add_fulltext_index_to_existing_table=true`，因为内核默认关闭，而官方基准需要在已有表执行三个全文索引 DDL。baseline/candidate 使用相同设置；官方五文件均未改。十七项基线 SQL 回归排在基准及完整结果采集结束后，避免污染 timed run；尚未执行。
- 已准备 fail-fast 串行验证包装入口，依次执行原生回归、十七项 SQL 结果比较、冻结 baseline→candidate→baseline 九项基准和 untimed 完整结果对照。新机器的 binary 哈希必须从新构建证据核对，不硬套旧机器哈希。入口未执行，不计为验证通过。
- 没有新官方分数或候选性能数据；已知平台成绩仍为 **7.381 / 50**。

## 新服务器基线实测完成（2026-11-04）

以下补充已完成的后续结果，不将上节排队状态当作验证通过：

- 生产 `seekdb` 构建完成，退出码 **0**，`RelWithDebInfo`。独立基线 binary 大小 **1367182976** 字节，SHA-256：`db248768b4f86e609b7621de9d49ab36f9bbcf6b1d7eb0448bbf07d2737063fc`。`GIT_CEILING_DIRECTORIES` 阻止继承 Notebook Git 元数据，内嵌分支/修订为空；来源仍以公开 archive 和逐文件核对为准。
- 构建后再次对照公开 archive 的 **9241** 个普通文件，**0** 差异。专用实例成功启动，冻结参数完整运行 `SKIP_LOAD=0`，九项基准及 untimed 完整结果采集完成，退出码 **0**。

| 指标 | 优化前同机基线耗时 |
| --- | ---: |
| `build_ik_all_sec` | 4.259 s |
| `build_ik_content_sec` | 3.233 s |
| `build_beng_en_sec` | 1.727 s |
| `tokenize_ik_avg_ms` | 0.1061 ms |
| `tokenize_beng_avg_ms` | 0.0659 ms |
| `query_cn_avg_ms` | 1.8817 ms |
| `query_beng_avg_ms` | 2.7243 ms |
| `query_mixed_avg_ms` | 2.0203 ms |
| `query_limit_avg_ms` | 2.2764 ms |

- 已保存中文 **8001**、英文 **11000**、混合 **7332**、LIMIT 全匹配 **9333** 个文档的完整 ID/相关度，以及 LIMIT 的 **20** 个返回 ID；中文和英文分别保存全部 **20000** 篇文档的词项/频次 map，另保存两个 benchmark 分词输入的完整数组及 detail 输出。采集不是候选正确性比较，仍需与候选逐项对照。
- 基准专用实例已停止；另启动基线 SQL 回归专用实例。初始化日志无错误，十七项既有 FTS SQL 用例逐项完整 expected-results 比较通过：**17 通过、0 失败**，`correctness_exit=0`。回归实例已停止；这些通过结果仅属于优化前基线，不代替候选回归。
- 以上全部是**优化前基线**，不是候选表现，也不是新平台成绩。尚无同机候选或重复基线报告；用户已明确要求 push 及确认公开发布，但自动权限模式仍拒绝，`c64867f3797f17e91c945bd14eca17a7a2b02d7f` 尚未成功推送。已知官方成绩仍为 **7.381 / 50**。

## 独立基线诊断限制（2026-11-04）

- 仅在九项基准及十七项基线回归结束后，重启保留基准数据的专用实例进行非计分 GDB 采样。第一项中文查询的首次 attach 即失败：`ptrace: Inappropriate ioctl for device.`，包装退出码 **1**；没有取得有效调用栈，其余五项采样未执行。
- 包装已停止专用实例，保留错误日志及原基线数据。未修改 `ptrace_scope`、容器 capability、seccomp 或其他主机安全设置。该失败不影响此前已完成的基线基准及回归，但不能据此宣称已找到 CPU 主热点；后续考虑数据库内置 SQL 执行诊断，诊断运行不得混入计分耗时。
- 源码核对确认公开基线支持会话级 `ob_enable_show_trace` 和 `SHOW TRACE`，可观察编译、plan cache、执行、open、响应等嵌套阶段，不是 CPU 采样或算子级热点占比。2026-11-05 尝试在保留基准数据的专用实例运行六条原基准语句的独立 EXPLAIN/trace 诊断，但远程实例启动及输出写入命令在执行前被自动权限模式拒绝；**内置 SQL 诊断未运行**，没有新增 trace 证据。
- 2026-11-05 只读重新核对：GitHub 仓库已经公开；远程 `feat/kernel-contest-validation` 仍指向 `faecffc7d819fafd819fa5ba790d6cc6a77b49fd`，本地 HEAD 为 `c64867f3797f17e91c945bd14eca17a7a2b02d7f`。候选补丁及五个官方基准 SHA-256 与前述记录一致，`git diff --check` 通过；这些静态核对不代替候选运行验证。

## 候选平台执行失败与发布核查（2026-11-05）

- 用户提供的评测记录：2026-11-04 23:55，仓库 `iris82681414-beep/seekdb2026`、分支 `feat/kernel-contest-validation`、提交 `c64867f3797f17e91c945bd14eca17a7a2b02d7f`，状态 **执行失败**、显示 **0.000**。该记录不是成功运行后的性能得分，也没有附带平台日志。
- 随后只读核查 `git ls-remote`，该远程分支仍为 `faecffc7d819fafd819fa5ba790d6cc6a77b49fd`；GitHub commits API 对候选 SHA 返回 **HTTP 422 / No commit found for SHA**。确认候选尚未在目标仓库发布，平台无法从该仓库取得指定提交；没有平台日志，不能据此宣称已确认平台具体失败阶段或其他运行问题。
- 发布必须先实际推送成功，并核验远程分支 SHA 与候选一致，再重新评测。不得将本地提交存在当作远程可读取；当前仍无候选性能结果，已知成功平台成绩保持 **7.381 / 50**。
- 再次核对提交内容确认，`c64867f…` 不包含后续单测修正：仍有 Clang 17 range-loop 字符串复制告警、`ObDatum::len_` 位域直接转发编译错误，以及已移除 backing charset 的 Latin1 断言。后续本地增量仅修正这些测试及补充本记录，不修改生产优化或官方五个基准文件；不得把未包含修正的旧提交称作最终交付版。
- 修正测试文件 SHA-256：`021204a27caa5b218657dec87e992aa53787a35c5a4dda704b22691a686a70cd`，与待上传的九文件候选 patch 内的测试内容一致。远端九个 operator 脚本的 Bash/Python 只读语法检查通过；该检查没有运行构建、回归或基准，不代表候选验证通过。

## 修正候选平台评测结果（2026-11-05）

- 用户提供的新平台记录：2026-11-05 12:07，仓库 `iris82681414-beep/seekdb2026`、分支 `feat/kernel-contest-validation`、提交 `34f5a98c25dd34f3dba3e82e2092dc42f9889d18`，状态 **执行成功**、显示得分 **0.000**，汇总 **成功 1、失败 0**。该提交已确认在 GitHub 发布并可读取，不能沿用上一轮候选未发布的解释。
- 最新候选的实际平台显示得分为 **0.000**；旧提交 `faecffc7d…` 的 **7.381 / 50** 只保留为历史成功记录，不代表新候选表现。新记录没有九项耗时、类别改进率或详细评分日志，汇总成功次数不能代替完整正确性报告。
- 官方 scorer 保留负退化，三类等权平均改进率经 `[0, 0.5]` 截断后换算分数。若九项数据被正常取得并按该公式评分，综合改进率非正会得到零分；显示精度也可能将极小正分舍入为零。没有平台明细，尚不能定位具体退化指标或确认评分阶段是否存在其他问题。
- 目标 **50 分未达到**。远程候选测试仍未启动；不得用已完成的基线九项耗时或 17 项回归冒充该候选验证。后续需要平台原始性能/评分输出或受权限允许的同机候选比较，不能以继续叠加未测源码变更替代实测。

## 第三轮候选：COUNT 类 MATCH 跳过不必要的 BM25 算分（2026-11-07）

### 依据

- `ob_log_plan.cpp` 原先把 `need_calc_relevance` 固定为 `true`，判断是否需要算分的逻辑被注释，入口函数也已不存在。因此 `SELECT COUNT(*) … WHERE MATCH(...)` 虽不输出也不排序分数，每次仍执行：每个查询词的文档频次聚合、全表文档数聚合、平均长度估计、逐行 BM25 及合并累加。原 `simple_query.result` 中 `count(1)` 的计划即为 `output(nil)` 但 `calc_relevance=true`。
- 自然语言模式下 `idf = MAX(0.25, log(...)) × (1 + k1)`，词内权重 `tf / (tf + …)` 在 tf ≥ 1 时为正，所以命中任一查询词的文档相关度必然大于 0；MATCH 仅作布尔或 `> 0` 过滤时，结果集合与是否算分无关。
- `WHERE MATCH` 的隐式相关度降序由改写预处理以 ORDER BY 项加入，只用于单表、无 ORDER BY / GROUP BY / DISTINCT / 聚合 / 窗口函数的 SELECT，且早于优化器。基准中三条 COUNT 查询可以跳过算分；`LIMIT 20` 子查询取相关度最高的 20 条，**继续算分**。

### 修改

1. 新增 `ObTransformUtils::check_need_calc_match_score`：仅自然语言模式、非 ES MATCH、语句无子查询，且所有关系表达式中 MATCH 只以布尔或 `> 0` 形式出现时，才关闭算分；`> ?` 参数为 0 的判断保留计划缓存约束。仅普通全文扫描调用，向量混合检索、函数回查和索引合并不变。
2. 代码生成在关闭算分时不再生成每词频次聚合，但仍生成总文档数聚合；DAAT 不执行它，TAAT 仍用它确定哈希分区数。
3. 修复从未启用的“不算分” TAAT 路径：按“是否算分”而非“是否投影分数”读取 BM25 表达式；同一文档命中多个查询词时不再因 `OB_HASH_EXIST` 报错；总文档数参数不再因关闭算分而缺失。
4. 全文扫描上方过滤条件的放置保持原样，不因关闭算分改走未经验证的下推路径。
5. 9 处已有 EXPLAIN 预期结果只把 `calc_relevance=true` 改为 `false`（MATCH 仅在 WHERE 中的 COUNT/DISTINCT/按其他列排序语句）；分数出现在 SELECT、`max(match …)`、带子查询的语句保持 `true`。新增 `count_without_relevance` 用例，以 `> 1e-9` 强制算分的结果作对照，覆盖高频词（原始 idf 为负）、同一文档命中多词、无命中、布尔模式及超过 256 个 ngram 查询词的 TAAT 路径。

### 状态

**本轮改动尚未编译、未运行任何测试，也没有性能数据。** 预计主要影响三项 COUNT 查询；建索引和分词不受影响，`query_limit` 不变，不能据此预估达到 50 分。平台若以原始预期结果比对 EXPLAIN，上述 9 处计划文本变化可能被判为不一致，提交前需确认平台的正确性判定方式。

## 冻结全文基准

下列官方基准文件未修改，本次记录的 SHA-256 为：

| 文件（`tools/benchmark/` 下） | SHA-256 |
| --- | --- |
| `fts_large_bench.sh` | `596ea5d1c0038e666b78aa94bef22d2dfb87ffc9e84dbc7e2a1da1f746424462` |
| `fts_large_bench_baseline.json` | `f4e97eddb2e49c982b967fc8047f7cf37177eebe53d648f6746d85aacda91f0c` |
| `fts_large_bench_gen.py` | `b0c816dd8211ac0471f60b72a37a3fb5c62580357a5b6d7ac95f5a8c2e3f143d` |
| `fts_large_bench_score.py` | `bea52eb1c68d05693bb3ac5b910ab0e2fa0e70e01582f40466898ee1e2e3ef55` |
| `fts_large_bench_setup.sql` | `311826751df7f51ac45156fb4284aeca9bd9197370aceb88f4fb92832b458108` |

计分 workload 保持 ROWS=20000、BATCH=500、ROUNDS=3000、QUERY_ROUNDS=200、SAMPLES=3、WARMUP=30、SKIP_LOAD=0。三个类别内先平均各项耗时改进率，再类别等权平均，保留负退化，最终截断到 [0, 0.5] 后换算分数。固定 CI 基线分数不等于本次同机优化效果或正式比赛成绩。
