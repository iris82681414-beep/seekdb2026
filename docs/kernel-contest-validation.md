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
- 原始全文基线、性能瓶颈 profile、候选优化及同机 baseline→candidate→baseline 验证。

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
