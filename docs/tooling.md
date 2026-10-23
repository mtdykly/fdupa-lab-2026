# FDULang 工具说明

安装和构建方法见[项目首页](../README.md)。以下命令在项目根目录执行，以 Linux 为例；其他系统按构建结果替换可执行文件路径即可。

## 运行程序

```sh
./build/fdulang check examples/03_recursion.fdulang
./build/fdulang dump-cfg examples/02_relation.fdulang
./build/fdulang run examples/03_recursion.fdulang
./build/fdulang run examples/03_recursion.fdulang --input inputs.txt --json
```

`check` 检查词法、语法和静态语义，并生成 CFG。`dump-cfg` 打印各函数的变量、节点、边及调用点和 abort 编号。`run` 解释执行 CFG，默认从控制台读取 `input` 所需的整数，也可以用 `--input` 指定输入文件。

`check`、`run` 和 `analyze` 都支持 `--json`。例如，一次执行到 abort 时，结果中的 `abort_id` 给出该语句的编号；静态分析则用 `may_abort` 列出可能执行到的 abort。

## 运行分析

```sh
./build/fdulang analyze tests/labs/cases/common/19_relation_sum.fdulang --domain interval
./build/fdulang analyze tests/labs/cases/common/19_relation_sum.fdulang --domain octagon
./build/fdulang analyze examples/03_recursion.fdulang --domain octagon --mode inter --k 2 --json
```

`--domain` 选择 `interval` 或 `octagon`，默认是 `interval`；`--mode` 选择 `intra` 或 `inter`，默认是过程内分析。过程间分析用 `--k` 指定调用串长度，默认是 1。k=0 时，同一函数的所有调用共用一个上下文。

JSON 结果包含完整的 `abort_sites`、预测集合 `may_abort`、分析配置 `config` 和求解统计 `stats`。`iterations`、`joins`、`widenings` 记录迭代、合流和扩大操作的次数；过程间分析还记录 `contexts`、`summary_updates` 和 `dependency_notifications`，分别表示上下文数、返回摘要更新数和调用者重新调度数。

以下选项可用于调试：

| 选项 | 用途 | 默认值 |
| --- | --- | --- |
| `run --max-steps` | 最多执行的 CFG 节点数 | 1000000 |
| `run --max-depth` | 最大调用深度，包含 main | 10000 |
| `analyze --widen-delay` | 执行 widening 前允许的更新次数 | 2 |
| `analyze --max-iterations` | 最大分析迭代次数 | 1000000 |
| `analyze --max-contexts` | 过程间分析的最大上下文数 | 10000 |

上述 `--max-*` 选项都可设为 0 来关闭对应限制。八边形域另外支持最多 128 个变量槽位，包含 CFG 的临时变量。正式测试使用默认分析配置，比较结果时应保持配置一致。

## 运行测试

语言实现测试检查前端、CFG 和解释器：

```sh
python3 tools/run_language_tests.py
```

三个 Lab 的自测命令如下：

```sh
python3 tools/run_lab_tests.py --domain interval
python3 tools/run_lab_tests.py --domain octagon
python3 tools/run_lab_tests.py --mode inter --domain all --k all
```

脚本会构建项目，运行所选域和分析模式对应的实现检查，然后分析测例并逐项比较参考告警集合。过程内使用公共集 `common.json`，过程间使用完整集 `all.json`。`--domain all` 分别评价两个域，`--k all` 分别评价 k=0、1、2。

Lab3 可以先用 `--domain interval` 或 `--domain octagon` 检查一个域；`--domain all` 检查两者。加上 `--all-checks` 会运行三个 Lab 的全部实现检查，分析测例仍按所选配置执行。

两个测试入口都支持 `--build-dir`、`--config`、`--generator` 和 `--jobs`。Lab 入口还可以保存逐例结果：

```sh
python3 tools/run_lab_tests.py --mode inter --domain all --k all \
  --json-out build/inter-results.json --csv-out build/inter-results.csv
```

已经构建好程序时，可以单独运行评价器：

```sh
python3 tools/evaluate_labs.py --binary build/fdulang --domain interval \
  --expected tests/labs/expected/interval-intra.json
```

省略 `--expected` 时，只按 ground truth 计算指标。测例、参考结果和指标的含义见[分析测例说明](../tests/labs/README.md)。

## FDULang 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 检查、CFG 打印或分析成功，或程序正常结束 |
| 1 | 具体执行到 abort |
| 2 | 运行时错误，如整数溢出或输入格式错误 |
| 3 | 执行或分析超过资源限额 |
| 64 | 命令行参数错误 |
| 65 | 源程序未通过前端检查 |
| 66 | 无法读取源文件或输入文件 |
| 70 | 工具内部错误 |

分析成功时，即使 `may_abort` 非空，退出码也是 0。
