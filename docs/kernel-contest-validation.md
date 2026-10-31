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

新增 `unittest/storage/test_fts_token_pipeline.cpp`，目标 `//unittest/storage:test_fts_token_pipeline`。覆盖旧分组算法对照、非 1 初始频次、collation 等价 key/最后代表值、大小写、停用词/长度边界、非分组重复及非法输入、未初始化 map、默认/持久 analyzer 对照、过滤/聚合开关、BENG 输出在后续 token、EOF、parser 销毁及原输入修改后的有效性。包含长 token、UTF-8/Latin1、非 ASCII 与坏字节输入对照，以及两种 allocator 分离和 analyzer 重初始化。

该目标为 Linux-only 静态链接 C++ 单测；生产 translation units 已在原 inventory 注册，不新增生产清单。CMake inventory reader 原先看到 `unittest/storage/BUILD.bazel` 就要求统一测试农场宏，会拒绝此独立 cc_test 包。本轮最小修正为显式 standalone `cc_test` 保持 Bazel-owned，原统一宏数量与字段校验保留。没有为构建脚本新增测试。

### 本轮实际验证与限制

- `git diff --check` 已通过；接口、调用点和 allocator 生命周期已做静态核对。官方基准五文件 SHA-256 再核对一致。
- 尝试运行源码 inventory 生成命令，但执行权限检查在运行前拒绝：需要显式授权执行目标仓库脚本。**该命令未执行，未生成可供核验的本轮 inventory**，没有换工具或主机绕过。
- 用户询问已有 SSH 后，对已授权主机做只读连接检查，返回 `Connection refused`，退出码 255；未进入远程主机，未改变现有部署。不在仓库发布连接地址。
- 按「不要编译，直接 push」要求，**本轮生产源码与新增 C++ 回归均未编译、未执行**。没有候选生产 binary、候选 SQL 结果或新性能报告；IDE 缺失 include 配置的诊断不算编译证据。

推送后仍须平台或之后单独授权环境验证运行正确性与九项性能。不得使用旧 binary 冒充候选，不能只核对命中 COUNT。官方基准会重建 `fts_large_bench`，只能在专用实例执行，不能对保留部署运行。

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
