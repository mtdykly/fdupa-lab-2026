# FDULang 语言实现测试

本目录只验证词法、语法、静态检查、CFG 转换和具体解释执行。它不是 Lab 的抽象解释测试集，不包含抽象域精度基线，也不计算准确率、误报率或漏报率。

测试使用 C++ 标准库编写的独立程序 `fdulang_language_tests`，不依赖第三方测试框架。程序每例输出 PASS 或 FAIL，汇总通过和失败数；任一失败返回 1，过滤条件未匹配到用例返回 2。

```text
fdulang_language_tests
fdulang_language_tests --list
fdulang_language_tests integer/
fdulang_language_tests cfg/
```

用例在 `main.cpp` 内使用短源程序定义，便于同时阅读程序与预期行为。前端错误主要比对错误类别与关键源码位置，避免提示文字改进导致无关失败。执行用例通过具体结果、输入消费顺序和程序内部的条件检查验证行为，不把待验证分析器当作 oracle。

覆盖范围包括整数边界与每步溢出、UTF-8 和源码位置、词法和表达式类别、名称与作用域、完整输入 token、调用参数和返回、递归与相互递归、main 的嵌套调用、求值顺序、终止传播、资源限制，以及静态 ID 和 CFG 约束信息。

Lab 用例位于独立的 `tests/labs/`，使用自己的清单、ground truth、运行工具与结果统计。语言测试中的 runtime_error、前端错误或 resource_limit 不得加入 Lab 的 abort 分类分母。
