# FDULang 示例

这些程序展示 FDULang 的语法和执行行为。

| 文件 | 内容 | 示例输入与结果 |
| --- | --- | --- |
| [00_minimal.fdulang](00_minimal.fdulang) | 正常结束与不可达 abort | 无输入，正常结束 |
| [01_input_loop.fdulang](01_input_loop.fdulang) | 输入、分支、循环与局部变量 | 输入 `3`，正常结束 |
| [02_relation.fdulang](02_relation.fdulang) | 变量之间的和约束 | 输入 `4`，正常结束 |
| [03_recursion.fdulang](03_recursion.fdulang) | 递归累加 | 输入 `4` 正常结束；输入 `5` 执行 A1 |
| [04_mutual_recursion.fdulang](04_mutual_recursion.fdulang) | 相互递归 | 输入 `4` 正常结束；输入 `5` 执行 A1 |
| [05_evaluation_order.fdulang](05_evaluation_order.fdulang) | 实参从左到右求值 | 输入 `3 2` 正常结束；输入 `2 3` 执行 A1 |
| [06_termination.fdulang](06_termination.fdulang) | 调用内部 halt | 无输入，正常结束，后续代码不执行 |

在项目根目录运行，例如：

```sh
./build/fdulang run examples/03_recursion.fdulang
```

程序执行到 `input` 时，在控制台输入整数即可，多个整数之间以空白分隔。语言定义见[语言规范](../docs/language.md)，Lab 使用的分析测例见 [tests/labs/](../tests/labs/README.md)。
