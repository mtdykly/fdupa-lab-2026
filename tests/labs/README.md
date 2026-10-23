# Lab 分析测例

Lab1 和 Lab2 使用相同的公共集，Lab3 在公共集上增加调用和递归测例。

| 清单 | 程序数 | abort 数 | 可达 | 不可达 |
| --- | ---: | ---: | ---: | ---: |
| `common.json` | 28 | 42 | 15 | 27 |
| `interprocedural.json` | 18 | 24 | 8 | 16 |
| `all.json` | 46 | 66 | 23 | 43 |

`cases/common/` 从常量、输入和分支开始，逐步涉及合流、循环、变量关系和函数调用。`cases/interprocedural/` 的内容见[过程间测例说明](cases/interprocedural/README.md)。`all.json` 合并前两份清单，复用相同的源文件和真值。

每份清单记录源文件、源码哈希、全部静态 abort 的位置和 ground truth。可达表示存在一组输入使程序执行到该 abort；相应条目还给出一组具体输入。真值依据程序语义确定。

`expected/` 保存参考实现的预测集合：

| 文件 | 使用的测例和配置 |
| --- | --- |
| `interval-intra.json`、`octagon-intra.json` | 公共集，过程内分析 |
| `<domain>-inter-k0.json`、`<domain>-inter-k1.json`、`<domain>-inter-k2.json` | 完整集，过程间分析 |
| `<domain>-intra-all.json` | 完整集，过程内分析，用于与 Lab3 对照 |

其中 `<domain>` 为 `interval` 或 `octagon`。参考预测采用上近似，可能包含误报。完成实验的标准是逐个程序的告警集合与对应参考结果一致，并且没有漏报。

## 运行与比较

在项目根目录运行：

```sh
python3 tools/run_lab_tests.py --domain interval
python3 tools/run_lab_tests.py --domain octagon
python3 tools/run_lab_tests.py --mode inter --domain all --k all
```

入口先运行当前域和模式对应的实现检查，再评价测例。Lab3 可选择单个域或单个 k；例如 `--mode inter --domain interval --k 1`。`--all-checks` 额外运行三个 Lab 的全部实现检查。

只评价已构建的分析器，可以运行：

```sh
python3 tools/evaluate_labs.py --binary build/fdulang --domain octagon \
  --mode inter --k 2 --expected tests/labs/expected/octagon-inter-k2.json \
  --json-out build/lab3-results.json --csv-out build/lab3-results.csv
```

`--json-out` 和 `--csv-out` 保存逐例告警、统计与集合比较结果。省略 `--expected` 时只比较 ground truth；传入时还比较参考告警集合。只比较汇总准确率不足以判断两个实现的结果是否相同。

评价器默认在过程内模式下读取 `common.json`，过程间模式下读取 `all.json`。例如，要比较两个模式在完整集上的结果，可通过 `--manifest tests/labs/all.json` 选择完整集，同时为过程内分析指定对应的 `<domain>-intra-all.json`。

## 指标

统计单位是静态 abort 语句。TP 表示正确报告的可达 abort，FP 表示误报的不可达 abort，TN 表示正确排除的不可达 abort，FN 表示漏报的可达 abort。

| 指标 | 公式 |
| --- | --- |
| 准确率 accuracy | `(TP + TN) / (TP + FP + TN + FN)` |
| 精确率 precision | `TP / (TP + FP)` |
| 召回率 recall | `TP / (TP + FN)` |
| 误报率 FPR | `FP / (FP + TN)` |
| 漏报率 FNR | `FN / (TP + FN)` |

不同分析配置分别统计。评价器退出码 0 表示全部测例完成、没有漏报，且指定的参考集合均匹配；1 表示运行失败、漏报或集合差异；2 表示参数或数据文件错误。

本目录中的 C++ 单元测试和 Python 命令行、评价器测试用于检查实现，它们不参与准确率计算。语言实现测试位于相邻的 [language/](../language/README.md)，使用独立入口。
