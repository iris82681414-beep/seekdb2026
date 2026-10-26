# 大赛指定 Bug #1477 修复材料（私有准备）

对应 [oceanbase/seekdb #1477](https://github.com/oceanbase/seekdb/issues/1477)。2026-10-27 核对时，Issue 为 open，带有 `2026-competition`、`type: bug`、`help wanted` 标签。选题符合[大赛 Bug 修复说明](https://oceanbase.yuque.com/org-wiki-obtech-vh7w9r/hc3pio/xom0q8ni2ow8t9py)的标签要求。本文及代码仅保存在用户私有仓库，尚未创建官方 PR，也未提测；不据此宣称获得基础题分数。

## 复现与原因

```sql
SELECT 1 FROM dual
WHERE IFNULL((SELECT FALSE), !(!(1 LIKE 'x')));
```

预期为空结果；修复前原生实例返回 `ERROR 4016 (HY000): Internal error`。将双重 NOT 改为单重 NOT，或将子查询改为字面值 FALSE，则正常返回空结果。

`ObExprNot` 的结果类型为 int32，`LIKE` 的结果类型为 int64。`try_create_bool_expr` 将 LIKE 视为已有布尔表达式，可直接返回原 LIKE 节点。删除偶数层 NOT 后，替换节点的值仍是 0、1 或 NULL，但其结果类型变宽。已按原类型完成推导的 IFNULL 再次推导时，需要给标量子查询已有的隐式转换再次加转换，从而触发重复隐式转换检查。

## 实现范围

在 `ObTransformSimplifyExpr` 两处删除偶数层 NOT 的位置，完成新节点类型推导后，使用既有的 `ObTransformUtils::add_cast_for_replace_if_need` 保留原表达式类型：

- `do_push_not`：一般 NOT 链简化。
- `inner_remove_dummy_nvl`：IFNULL/NVL 第一个参数的 NOT 链简化。

数字输入继续由原布尔转换逻辑归一化，NULL 保持 SQL 三值语义。替换只在构造、类型推导及必要转换全部成功后发生。

独立分支为 `fix/issue-1477-not-result-type`，基于官方 `master` 的 `2d6a63248d14b65ae0ea959a7dc266512094d7c5`。独立提交 `34b696a50198bdb8d5280f3f881ede6405d96625` 仅修改一个 C++ 文件并新增一对原生回归文件；已推送用户私有仓库。比赛答题分支上的整合提交为 `d38f79a7c793f4dfb60cfffce501bcda7a256042`。

## 验证结果

| 验证 | 修复前 | 修复后 |
| --- | --- | --- |
| 新增 `subquery.ifnull_double_not` | 首条复现语句报 4016，测试失败 | 通过 |
| 原版 `datatype.bool` | 通过 | 通过 |
| 原版 `subquery.subquery_sj_firstmatch` | 通过 | 通过 |
| 576 组独立语义及列类型码对照 | 522 组通过，54 组报错 | 576 组全部通过，零错误 |

整合版本另完成 29 项既有原生用例：AI 函数 12、AI 列 10、File SQL 2、FTS 5，全部通过。首次 File SQL 的两项在复制数据阶段因本机 SSH 配置失败，查询未执行；补齐此前已使用的临时、仅限本机的 SSH 及主机指纹配置后，只重跑这两项并完成 FTS。首轮失败日志保留，测试源码、期望输出及产品代码均未因此修改；临时授权和密钥目录在结束时已清理。

新增 mysqltest 覆盖题面复现及两个对照，TRUE/FALSE/NULL 子查询、三/四重 NOT、数字 2/-2/0/NULL、IFNULL 两个参数，以及表字段上的 LIKE 和数字输入。期望结果按 SQL 语义编写，修复前后使用同一份文件。

独立对照由 12 种条件、3 种子查询值、4 种 NOT 链长度、2 个参数位置、投影及 WHERE 两种上下文构成。每组同时与 Python 计算的 SQL 三值结果及 `NO_REWRITE` 查询比较；576 组参考查询均符合预期。修复后返回值和线协议列类型码均与参考查询一致。

Linux `RelWithDebInfo` 整合构建成功，耗时 63.71 秒。已逐文件核对全部 103 个相对冻结基线有变化的构建输入，服务器与整合提交完全一致；源码清单 SHA-256 为 `45c830572a9f73542127a2b514d505440bbb3289e327cb303dd6d9cc6523b33f`。对照使用同一个隔离实例，主测试实例保持运行；验证结束后隔离实例已停止。对照二进制 SHA-256 为 `e6be5e0b3adebd43a35905d9e1dab459aa03ac08ef7577b58c10ad5deeafdc31`；修复后为 `520e668f3134e388c02f2670ae4faa9e015fc27bf808b7de7e9c93753ed5bb4d`。

构建和运行验证的环境是已含比赛实现的答题分支，而非完整的最新 master 构建。两分支的相关简化实现、NOT/LIKE 类型推导及重复转换检查一致，所用类型保留辅助函数也一致；上述记录不替代未来官方 master CI。独立分支尚未执行完整 master 构建。

详细结果、逐查询数据、原始日志及源码摘要保存在本地 `.competition/issue1477/`，由 Git 本地排除规则排除。最终汇总为 `results/issue1477-final-validation.json`；原始前后对照为 `results/issue1477-validation.json`，包含二进制、源文件和原生回归的 SHA-256；源码核对记录为 `results/issue1477-source-check.json`。

## 后续公开时的材料

可用英文标题：`fix(sql): preserve NOT result types during simplification`。PR 描述应关联 #1477，说明上述原因、两个替换位置和修复前后验证结果，并如实说明整合构建的范围。公开前仍需按届时规则填写本队验证行；当前保留私有，不创建 PR，不向平台提交。
