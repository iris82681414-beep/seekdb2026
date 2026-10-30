# 文件即数据库：提交范围与实测记录

题面：[文件即数据库](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/gy2a66vag0028msk)，
读取到的内容更新于 2026-10-26。评分表列出 8 项、合计 50 分；COPY 段落中
仍有旧的“30 分”文字，本记录不据此预测平台分数。

## 提交范围

本分支保留报名仓库已有的文件 SQL 主体实现，以及已经验证的 AI 修复。
本次补充边界测试、解析器测试，并修复 JSONL 字段名包含 NUL 时仍能进入
动态 SQL 列元数据的问题。CSV 和 Parquet 已拒绝此类列名，JSONL 现在同样
在解析输入记录时返回错误；字段值中的合法 JSON 转义不受此检查影响。

| 题面要求 | 实现位置与行为 |
| --- | --- |
| SQL 接口、命名参数、别名、DESCRIBE、glob | `src/sql/parser/sql_parser_mysql_mode.y`、`src/sql/resolver/dml/ob_file_sql_resolver.cpp`；动态列进入原生函数表 |
| CSV | `rust/file-scan/src/csv.rs`；UTF-8、LF/CRLF、引号、转义、多行、NULL、坏行定位 |
| JSONL / NDJSON | `rust/file-scan/src/jsonl.rs`；扁平对象、字段并集、缺失 NULL，拒绝嵌套结构及无效列名 |
| Parquet | `rust/file-scan/src/parquet_reader.rs`；Footer Schema、多 Row Group、可空列、Snappy 和列裁剪 |
| 类型推断 | `rust/file-scan/src/lib.rs`；全量 CSV/JSONL 推断、确定性宽化、七类类型、原始文本保留；`sample_size` 仅接受 `-1` |
| 单文件 SQL 和 JOIN | `src/sql/engine/basic/ob_function_table_op.cpp`、`ob_file_scan_utils.cpp`；seekdb 原生执行表达式、过滤、聚合、排序和 JOIN |
| 安全和稳定性 | `ob_file_scan_utils.cpp`；默认关闭、FILE 权限、运行时 `secure_file_priv`、逐级安全打开；Rust 侧检查文件变化与格式资源上限 |
| CSV COPY | 原生 INTO 输出路径及 `rust/file-scan/src/writer.rs`；表/查询导出、自定义 CSV 选项、读回、拒绝覆盖、错误清理 |

Rust 只解码文件格式和生成 CSV，不执行 SQL，也没有引入外部查询引擎。

## 2026-10-30 实测

环境：WSL Ubuntu 22.04 x86_64，仓库 SDK Clang 17，Rust 1.98.1。
SQL 使用新的 CMake Release 产物和隔离实例，`memory_budget=2G`、
`log_disk_size=4G`。测试实例关闭遥测，与比赛部署实例无关。

| 检查 | 实际结果 |
| --- | --- |
| 修复前 Rust 边界测试 | 12 项中 11 通过、1 失败；失败项为 NUL 字段名未被拒绝 |
| 修复后 `file_scan` Rust 单测 | 12 / 12 通过 |
| 新增 C++ 解析器测试 | 4 / 4 通过，覆盖 22 条有效/无效语句 |
| `cmake --build build_release --target seekdb -j6` | 成功，生成完整 seekdb 可执行文件 |
| 更新后的 `file_sql_basic` mysqltest | 通过，退出码 0 |
| 更新后的 `file_sql_queries` mysqltest | 通过，退出码 0；含 NUL 列名读取和 DESCRIBE 拒绝用例 |
| 独立 SQL 检查 | 28 项检查通过，包含设置步骤；生成 2,057 行 CSV，覆盖多个执行批次 |
| 原生执行计划 | 聚合为 `SCALAR GROUP BY -> FUNCTION_TABLE`；跨格式 LEFT JOIN 为 `HASH OUTER JOIN -> FUNCTION_TABLE × 2`，`rowset=16` |
| `git diff --check` | 通过 |

独立 SQL 检查验证了 CRLF、UTF-8 和多行引号字段、聚合、HAVING、表达式、
排序 LIMIT、JSON 字段并集及类型冲突、INNER/LEFT 跨格式 JOIN、全 NULL、
整数边界和小数秒；还检查了关闭开关、目录越界、符号链接逃逸、运行时收紧
路径策略、自定义 CSV COPY 读回、拒绝覆盖和查询失败后无残留文件。

新增 Rust 测试还覆盖正反行序的类型宽化一致性、扫描过程中修改文件后
拒绝已缓冲结果、空 Parquet 的 Footer Schema。已有测试覆盖两组 Snappy
Row Group、NULL、列裁剪和重扫。

### 测试运行方式的边界

原 mysqltest 的 include 使用 SSH/SCP 部署 fixture。本机没有本地 SSH
服务，运行时仅将 fixture 传输替换为本地复制，数据字节、SQL 和期望结果
均使用本分支版本；没有改写 SQL 执行逻辑或移除断言。

C++ 解析器测试使用 CMake 编译数据库中的头文件路径、与 Bazel 共享运行库
一致的宏定义，直接编译并链接已有 `liboceanbase.so`。本分支没有修改 C++
解析器生产代码；新的 Rust 文件读取代码由重建的完整 CMake seekdb 和原生
SQL 回归测试验证。新增 Bazel 测试目标已完成分析，但本机完整 Bazel 目标
构建在重建共享依赖时被主动停止，不能算作该目标构建通过。

没有运行平台隐藏测试、大文件 OOM 压力测试或完整 seekdb 全仓测试。
没有平台得分结果，不能将本地验证等同于 50 分通过。

## 复现入口

使用可用的系统 C/C++ 编译器执行 Rust 测试；如果 SDK 的 GCC 头文件布局与
宿主系统不匹配，可显式设置 `CC`、`CXX` 为 SDK Clang 17：

```bash
cargo test --locked --manifest-path rust/Cargo.toml -p file_scan --release --lib
```

解析器单测的标准 Bazel 目标：

```bash
python3 bazel.py build //unittest/sql/parser:test_file_sql_parser
./build_bazel/bin/unittest/sql/parser/test_file_sql_parser
```

文件 SQL 测试位于：

- `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_basic.test`
- `tools/deploy/mysql_test/test_suite/file_sql/t/file_sql_queries.test`

运行前应将 fixture 目录设置在 `secure_file_priv` 允许范围内；修改此全局
变量需使用实例 Unix socket。`file_sql_basic` 自行验证功能默认关闭，随后
在测试会话启用功能，并在结束时关闭。

最终提交应在已登记的私有 `seekdb2026` 仓库中选择 `feat/file-as-database`
分支及其完整 Commit id。该仓库的默认分支属于另一个项目，不能留空分支。
