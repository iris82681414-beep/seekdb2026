# AI 函数增强：实现与验证

本次工作在私有报名仓库的 `feat/ai-function-enhancements` 分支完成，基于已有
`2026-competition` 提交 `6140aabed3e78388de11088c61de26d8e7fdb967`。
保留队友已有实现及其他题目内容，没有修改已有远程分支或公开答题代码。

题面：[赛题一：AI 函数增强](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/psu9fks8dexldoum)。
六项共 60 分，以下记录是本地验证，尚无比赛平台确认的得分。

## 六项实现

六项主体实现已经存在于基线；本次核对并重新执行验证，另补充异常处理和测试。

| 子题 | 现有实现与本次覆盖 |
| --- | --- |
| AI_PROMPT | 保留 JSON Array/Object 与嵌套结构；TEXT/LONGTEXT 读取真实 LOB；重复替换、Unicode、转义、越界与整数溢出下标 |
| AI_EMBED 单行 | 正整数维度、默认维度推断；响应数量、非空向量、长度及有限数值元素校验 |
| AI_COMPLETE | 普通文本和结构化 Prompt、SQL NULL/JSON 配置、长文本；OpenAI 请求与非空字符串响应 |
| Ollama | Provider 大小写兼容、可省略/留空 Key、Endpoint 修改；原生非流式生成和 Embedding |
| AI_RERANK | 字符串/对象数组、字段选择、分批后全局索引、稳定排序及原始 ID；拒绝异常数量、索引、分数和结构 |
| AI_EMBED 跨行 | 按模型和维度合并输入；恢复乱序响应、传播 NULL、保留过滤/LIMIT/多执行批次中的行号 |

实现位于 `src/sql/engine/expr/ob_expr_ai/`；Endpoint 校验位于
`src/share/ai_service/ob_ai_service_struct.cpp`。
生产代码没有加入测试模型名、输入或响应的特殊判断。

## 本次修复

实现提交：`938cc81c88412d90138af7e80e59e245ea91ecbd`。

`ob_ai_func_client.cpp` 修复以下问题：

- 每次 HTTP 重试清空响应缓冲，避免传输中断留下的 JSON 前缀污染下一次响应。
- 批量调用耗尽重试后保留错误，检查 libcurl 的传输错误，失败时撤销新增响应。
- 使用 `CURLOPT_COPYPOSTFIELDS`，使请求体在临时 JSON 缓冲销毁后仍有效。
- 在释放 allocator 引用之前清理批量缓冲；处理分配/注册失败时的资源所有权，保留 curl header 链表。
- 为批量等待补充超时检查，并取消最后一次失败后的无效退避等待。

构建提交 `3f94d211c` 补齐当前分支缺失的 Bazel 头文件和模块依赖。
单测共享库链接将 Rust staticlib 从整体载入改为普通归档组查找，避免多个 crate
所包含的 Rust/CXX 公共运行时重复定义；没有使用忽略重复符号的链接参数。

## 实测结果

环境：Windows 工作区、WSL Ubuntu 22.04 x86_64、仓库 SDK Clang 17、
Bazel 8.2.1、Rust 1.98.1。隔离实例使用 `memory_budget=2G`、`log_disk_size=4G`。

1. 完整 `//src/observer:seekdb` 与 AI 单测构建成功。兼容性修复后，单测目标为
   `//unittest/sql/engine/expr:test_ai_functions`。
2. 新增 C++ 测试 **8 项全部通过**，无跳过。
3. AI suite 的 **14 项 mysqltest 全部通过**。第一轮有三项因缺少 MTR 使用的
   `admin` 账号而在连接步骤失败；补齐本地账号后只重跑这三项，全部通过，未改测试断言。
4. `git diff --check` 通过。

对照证据：同一分支修改前的客户端在持续 503 的 C++ 回归中返回 `0`（成功），
与期望的 `OB_CURL_ERROR=-4216` 不符；修复后返回错误。
修改前的 SQL 半截响应回归报 `3141 Invalid JSON text`；修复后重试成功，并通过行映射检查。
这些对照仅撤去本次客户端修复，保留了让分支可以构建的声明修复。

14 项 SQL 用例：

```text
ai_embed_batches
ai_embed_transport
ai_endpoint_concurrency
ai_function_enhancement
ai_model_ddl
ai_model_endpoint_ddl
ai_model_lower_case_name_mysql0
ai_model_lower_case_name_mysql1
ai_ollama_native
ai_prompt
ai_prompt_boundaries
ai_prompt_structured
ai_request_validation
ai_rerank_batches
```

其中批量向量回归实际检查 1,100 行、35 个 NULL，混合模型/维度、乱序响应、
过滤、倒序、LIMIT、跨请求维度变化及非法 index。
新增 LOB 回归检查超过 50,000 字符的模板及 Unicode、引号、换行、反斜线、
嵌套 Prompt 结构，并确认参数内的占位符不会被再次替换。

本分支原先没有题面列出的两份 C++ 测试文件；本次在相应路径新增测试，
通过显式 Bazel 目标运行，不能冒充平台未公开的原版单测。
综合 SQL 用例使用本地确定性协议 fixture 监听 8000 端口；其他用例使用仓库内
loopback fixture。未访问平台私有 Mock，没有测试真实模型效果、平台隐藏用例、
内存故障注入或完整 sanitizer 检查。

实际构建和单测命令：

```bash
python3 bazel.py --bazel=/path/to/bazel-8.2.1 build \
  --jobs=8 --local_resources=memory=12000 \
  --action_env=RUSTUP_TOOLCHAIN=1.98.1 \
  //src/observer:seekdb //unittest/sql/engine/expr:test_ai_functions
./build_bazel/bin/unittest/sql/engine/expr/test_ai_functions
```

本机原始日志、fixture 和执行脚本保留于仓库外
`D:\OceanBase2026\ai-functions-evidence`，未提交模型密钥、二进制或实例数据。

## 平台执行失败后的构建兼容修复

2026-10-28 提交 `7b88a039c0b3aceaaefc29db1e039bfb34bae357` 的平台记录显示
“执行失败 / 0.000”；尚未取得平台错误日志，不能断言平台失败原因。
本地排查确定了一个会阻止 CMake 配置的问题：新增的 `unittest/sql/BUILD.bazel`
会被 `tools/cmake/emit_bazel_source_inventory.py` 当作兼容 pretest 模块读取，
但独立 AI 测试没有该模块要求的 `seekdb_module_cc_test` 声明。
旧版执行 inventory 生成器实际返回错误：

```text
unittest/sql/BUILD.bazel must contain exactly one seekdb_module_cc_test call
```

修复将独立测试包移到 `unittest/sql/engine/expr/BUILD.bazel`，只向该包开放
SQL 测试接口；保留原 CMake 模块协议及其检查。独立 `main()` 也改为只在
Bazel 独立测试目标启用，避免和模块共用测试入口冲突。

本次补测：inventory 生成、`bash build.sh release`、带 CI 参数
`-DOB_USE_CCACHE=ON -DNEED_PARSER_CACHE=OFF` 的配置均通过；迁移后的
Bazel 目标通过全部 8 项 C++ 单测。此前 14 项 SQL 结果对应相同 AI 生产代码，
没有把它们写成 CMake 新产物的重测。当时尚未完成 CMake 全量编译或平台重测。

## 2026-10-30 完整 CMake 构建与回归补测

在代码提交 `3168c9b36b27a23c222844fc1a4dd2bb960ef837` 上补齐实际编译和运行验证。
本次没有改动六项功能的生产实现。对照私有仓库导出的 6,948 个源码、构建和测试文件，
Linux 构建目录与当前分支完全一致。

- `cmake --build build_release --target seekdb -j8` 完整构建通过，包含最终链接。
  使用前述带 `-DOB_USE_CCACHE=ON -DNEED_PARSER_CACHE=OFF` 的 Release 配置、
  SDK Clang 17 及 Rust 1.98.1。
- 重新执行独立 Bazel AI C++ 测试产物：8 项全部通过。
- 使用本次 CMake 生成的 `build_release/src/observer/seekdb` 启动新的隔离实例，
  `memory_budget=2G`、`log_disk_size=4G`；准备本地 MTR `admin` 测试账号后，
  上文列出的全部 14 项 AI mysqltest 一次运行全部通过。
- SQL 回归结束后正常停止实例；未使用 AMD 部署实例或修改其他任务的数据库。
- 本次使用本地协议 fixture，未访问平台隐藏 Mock；平台重新提测与得分尚未核验。

本次构建、运行日志和逐项结果保留于仓库外的
`D:\OceanBase2026\ai-functions-evidence`：`cmake-release-20261030.log`、
`cmake-runtime-20261030.log`、`validation-cmake-20261030.json`。

## 提测

在比赛平台选择已报名的私有仓库 `iris82681414-beep/seekdb2026`，
分支填写 `feat/ai-function-enhancements`，commit 使用该分支最新完整提交号。
仓库默认分支是其他产品内容，不能留空后依赖默认分支。
选择本题六个子题进行代码提测，以平台提测记录为准。
本题不需要公开 Fork、官方 PR、部署 UUID 或基础实践验证行。
