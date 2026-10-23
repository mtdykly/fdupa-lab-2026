# Lab1：过程内区间分析

本实验需要你实现一个区间分析器，在控制流图（CFG）上传播变量的可能取值，找出程序可能执行到的 `abort` 语句。

你需要完成 `src/labs/lab1_interval.cpp` 中的四项任务：

| 标记 | 函数 | 作用 |
| --- | --- | --- |
| `LAB1-JOIN` | `join_with` | 合并来自不同路径的状态 |
| `LAB1-WIDEN` | `widen_with` | 加速循环分析的收敛 |
| `LAB1-ASSIGN` | `assign` | 计算赋值后的区间 |
| `LAB1-ASSUME` | `assume` | 根据分支条件收紧区间 |

词法和语法分析、CFG 构造、解释器、工作队列以及其他区间辅助操作已经提供。四项任务都通过修改当前 `IntervalState` 完成，不需要修改公共接口。初始代码可以运行分析，但会丢失许多信息；单元测试用于检查这些待完成的操作。

完成实验后，你应当能解释一个抽象状态如何沿 CFG 传播、widening 如何加速循环分析，以及区间分析为什么会报告实际上不可达的 `abort`。

语言语法、安装和基本使用方法见[项目说明](../README.md)。本文命令以 Linux 为例，从含有 `CMakeLists.txt` 的项目根目录执行。

## 1. 从一个例子开始

先看 `tests/labs/cases/common/05_guarded_range.fdulang`：

```text
fn main() {
    let x = 0;
    input(x);
    if (x < 0) { halt; }
    if (x > 10) { halt; }
    if (x < 0) { abort; }
    if (x > 10) { abort; }
    if (x == 5) { abort; }
    halt;
}
```

程序读入 `x`。当输入小于 0 或大于 10 时，程序已经通过 `halt` 结束。因此，能执行到后面三个 `if` 的输入都在 `[0, 10]` 中。前两处 `abort` 不可达；第三处可以到达，例如输入 5。

区间分析用上下界记录这段推理。记 FDULang 整数的最小值为 `m`，最大值为 `M`：

| 位置 | `x` 的区间或状态 |
| --- | --- |
| `input(x)` 后 | `[m, M]` |
| 第一个 `if` 的假分支 | `[0, M]` |
| 第二个 `if` 的假分支 | `[0, 10]` |
| 第三、第四个 `if` 的真分支 | bottom，无法到达 |
| 第五个 `if` 的真分支 | `[5, 5]` |

这三个 `abort` 按源码位置编号为 `A1`、`A2`、`A3`，所以本例的分析结果应为 `{A3}`。

构建后可以查看它的 CFG 和分析结果：

```sh
./build/fdulang dump-cfg tests/labs/cases/common/05_guarded_range.fdulang
./build/fdulang analyze tests/labs/cases/common/05_guarded_range.fdulang --domain interval --mode intra
```

解释器处理一次具体执行；分析器则需要覆盖所有可能的输入。只要某条 `abort` 在某次执行中可达，分析结果就必须包含它。反过来，分析器报告的 `abort` 可能实际上不可达，这种情况称为**误报**。

## 2. 状态如何表示和传播

### 2.1 区间、top 和 bottom

区间 `[l, u]` 表示所有满足 `l ≤ x ≤ u` 的整数。例如，`[2, 5]` 表示 `{2, 3, 4, 5}`，`[3, 3]` 表示确定的值 3。

FDULang 使用有符号 32 位整数，本实验中：

```text
m = integer_min = -2147483648
M = integer_max =  2147483647
top = [m, M]
```

top 表示对取值没有限制。bottom 表示没有任何可能的执行，也就是这个位置不可达。二者的含义不同：所有变量均为 top 的状态包含所有可能取值；bottom 状态不包含任何取值。

一个 `IntervalState` 为函数中的每个 CFG 槽位保存一个区间。槽位既包括源程序变量，也包括表达式求值产生的临时变量。各个区间独立表示取值，例如：

```text
x: [0, 2]
y: [3, 5]
```

这个状态包含 `x = 0, y = 5` 等所有组合，无法表达 `y = x + 3` 这样的变量间关系。

类中用 `values_` 保存各槽位区间，用 `bottom_` 表示整个状态是否为 bottom。`bottom_` 为真时，`values_` 的内容不参与状态的含义。单个空区间用 `lower > upper` 表示；给任一槽位写入空区间，整个状态就成为 bottom。

### 2.2 框架已经做了什么

框架为每个 CFG 节点保存**执行该指令之前**的抽象状态。函数入口初始化为 top，其他节点初始化为 bottom，然后用工作队列反复传播：

1. 取出一个节点，复制它的入口状态。
2. 执行该节点的抽象操作，得到后继状态。分支节点分别计算真、假两条边。
3. 将后继状态与目标节点的已有状态合并；循环中的指定位置会使用 widening。
4. 如果目标节点的状态增长，将它加入队列继续处理。队列为空时，分析结束。

这里的“增长”指状态包含了更多可能取值，判断所需的 `leq` 操作已经提供。

各类节点的处理如下：

| CFG 节点 | 状态操作 |
| --- | --- |
| `Copy`、`Add`、`Sub`、`Neg` | 调用你实现的 `assign` |
| `Branch` | 调用你实现的 `assume`，分别处理两条边 |
| `Input` | 把输入目标设为 top |
| `Call` | 把返回值目标设为 top |
| `Abort` | 入口状态非 bottom 时记录该处告警 |
| `Halt`、`Return` | 结束当前路径 |

不同路径的合流和循环中的 widening 分别调用 `join_with`、`widen_with`。输入、调用、终止指令以及临时变量的清理由框架处理。

### 2.3 提供的类型和辅助操作

`Interval` 包含 `lower`、`upper` 两个端点。端点使用 `std::int64_t` 存储，以便计算加减结果；有效状态的端点范围仍为 `[m, M]`。

`Operand` 表示常量或槽位：`is_constant` 为真时读取 `constant`，否则 `variable` 是槽位编号。`Node` 中的 `target` 是赋值目标，`left`、`right` 是操作数；分支节点另有 `comparison` 和 `condition`。

下面的辅助操作已经实现，可以直接使用：

| 操作 | 含义 |
| --- | --- |
| `read(operand)` | 读取操作数区间；常量 `c` 返回 `[c, c]` |
| `write(variable, interval)` | 将区间与 `[m, M]` 取交后写入；结果为空则设整个状态为 bottom |
| `set_bottom()` | 将当前状态设为 bottom |
| `compatible(other)` | 将另一个抽象状态作为相同维数的区间状态访问 |
| `intersect_ranges(a, b)` | 求两个区间的交集 |
| `negate_compare(op)` | 求比较运算的逻辑补集，例如 `<` 变为 `>=` |
| `compare_constants(a, op, b)` | 判断常量比较是否成立 |
| `normalize_unary_guard(expr, op)` | 将支持的单变量线性条件整理为“变量 op 常量” |

区间状态声明位于 `include/fdulang/interval.hpp`，区间辅助代码位于 `src/domains/interval_support.cpp`。可以在任务文件中添加局部辅助函数，复用条件过滤等逻辑。

## 3. 实现四个操作

### 3.1 合并：join_with

`join_with(other)` 把当前状态与 `other` 合并，结果保存在当前状态中，`other` 保持不变。

如果两条路径上变量的区间分别是 `[l₁, u₁]` 和 `[l₂, u₂]`，合并结果应为包含它们的最小区间：

```text
[l₁, u₁] ⊔ [l₂, u₂] = [min(l₁, l₂), max(u₁, u₂)]
```

对每个槽位分别进行这一操作。bottom 不贡献任何取值，所以 `S ⊔ bottom = S`、`bottom ⊔ S = S`。

看 `11_join_hole.fdulang` 中的例子：

```text
fn main() {
    let choice = 0;
    input(choice);
    let x = 0;
    if (choice < 0) { x = 0; }
    else { x = 2; }
    if (x == 1) { abort; }
    halt;
}
```

两条分支上的 `x` 分别是 `[0, 0]` 和 `[2, 2]`，合并后得到 `[0, 2]`。实际取值只有 0 和 2，但一个区间必须把中间的 1 也包含进来。于是分析器会报告 `A1`，产生一次误报。

这说明 join 既要覆盖两条路径，也会受表示能力限制而丢失信息。

### 3.2 循环中的扩大：widen_with

循环会让同一节点的状态反复增长。例如，一个不断递增的计数器可能依次得到：

```text
[0, 0] → [0, 1] → [0, 2] → [0, 3] → …
```

FDULang 的整数范围有限，只做 join 最终也会稳定，但可能需要非常多次迭代。widening 用精度换取更快的收敛：某一端点只要向外扩张，就直接把它推到整数范围的边界。

`widen_with(other)` 中，当前状态是旧状态，`other` 是新的候选状态。对每个非空区间，设旧区间为 `[l, u]`，候选区间为 `[l', u']`，本实验要求：

```text
结果下界 = m，若 l' < l；否则为 l
结果上界 = M，若 u' > u；否则为 u
```

例如：

| 旧区间 | 候选区间 | widening 结果 |
| --- | --- | --- |
| `[0, 1]` | `[0, 2]` | `[0, M]` |
| `[0, 5]` | `[-1, 5]` | `[m, 5]` |
| `[0, 5]` | `[0, 5]` | `[0, 5]` |

bottom 的处理与 join 相同。结果需要包含两个输入；稳定的端点应当保留。

框架负责选择 widening 的位置和时机。默认 `widen_delay=2`：一个 widening 点初次从 bottom 变为可达后，允许两次严格增长使用 join，从第三次增长起使用 widening。传给 `widen_with` 的候选状态已经经过 join。

### 3.3 赋值：assign

`assign(target, op, left, right)` 根据旧状态计算右侧表达式的区间，再写入目标槽位。

设左、右操作数区间分别为 `[l₁, u₁]`、`[l₂, u₂]`，需要支持四类指令：

| 指令 | 含义 | 结果区间 |
| --- | --- | --- |
| `Copy` | `target = left` | `[l₁, u₁]` |
| `Neg` | `target = -left` | `[-u₁, -l₁]` |
| `Add` | `target = left + right` | `[l₁ + l₂, u₁ + u₂]` |
| `Sub` | `target = left - right` | `[l₁ - u₂, u₁ - l₂]` |

`Copy` 和 `Neg` 只使用左操作数。先读取旧操作数，再修改目标，这样 `x = x + 1` 才能正确处理。如果减法两侧是同一个槽位，`x - x` 应精确得到 `[0, 0]`；两个不同槽位即使区间相同，也不能这样处理。

算术还需要遵守 FDULang 的溢出语义：溢出使当前具体执行在该指令处发生运行时错误，无法到达后继。分析器传播的是能够正常完成这一步的结果。用 64 位整数计算端点，再交给 `write` 处理范围：

| 赋值前 | 指令 | 赋值后 |
| --- | --- | --- |
| `x: [2, 5]` | `y = x + 1` | `y: [3, 6]` |
| `x: [M - 1, M]` | `y = x + 1` | `y: [M, M]` |
| `x: [M, M]` | `x = x + 1` | bottom |

第二行仍有正常结果，因为 `x = M - 1` 时加一没有溢出。本实验的赋值只更新目标槽位，不要求反向收紧 `x`。已有状态是 bottom 时，赋值后仍为 bottom。

### 3.4 条件过滤：assume

`assume(branch, truth)` 保留可能满足所选分支的状态。`truth=true` 表示走真分支，`truth=false` 表示走假分支。

假分支应使用条件的逻辑补集。例如 `x < 3` 的假分支是 `x >= 3`。需要支持 `<`、`<=`、`>`、`>=`、`==`、`!=` 六种比较。

先处理 `branch.left`、`branch.right` 上的比较。它们是已经求值完成的操作数。对每一侧，求出满足比较的可能取值的最小区间；变量操作数需要写回，常量不需要写回。条件不可能成立时，把整个状态设为 bottom。

以下各行都从给定的原状态独立开始：

| 原状态 | 所选条件 | 过滤结果 |
| --- | --- | --- |
| `x: [0, 10]` | `x < 3` | `x: [0, 2]` |
| `x: [0, 10]` | `x >= 3` | `x: [3, 10]` |
| `x: [0, 10]` | `x == 5` | `x: [5, 5]` |
| `x: [0, 10]` | `x != 0` | `x: [1, 10]` |
| `x: [0, 10]` | `x != 5` | `x: [0, 10]` |
| `x: [5, 5]` | `x != 5` | bottom |

`x != 5` 无法在 `[0, 10]` 中挖去一个内部的点，所以该行保持原区间。排除端点则可以收紧区间。

两侧都为变量时，要同时考虑它们的取值。例如：

- `x: [-3, 5]`、`y: [2, 9]`，经过 `x == y` 后，两者都成为 `[2, 5]`。
- `x: [4, 10]`、`y: [0, 8]`，经过 `x < y` 后，得到 `x: [4, 7]`、`y: [5, 8]`。

两侧为同一槽位时，还要利用它们表示同一个值这一事实：`x < x` 和 `x != x` 不可能成立，`x <= x` 和 `x == x` 恒成立。已有状态是 bottom 时，条件过滤后仍为 bottom。

**利用源表达式收紧原变量。** 仅过滤比较指令的操作数，有时只能收紧临时变量。例如：

```text
if (x + 1 < 5) { ... }
```

CFG 会先计算 `t = x + 1`，再比较 `t < 5`。直接过滤只能得到 `t <= 4`。为了得到原变量的 `x <= 3`，分支节点还保存了 `condition`：它是源条件“左表达式减右表达式”的线性形式，与 0 比较。本例对应 `x - 4 < 0`。

在完成操作数过滤后，按以下规则使用 `condition`：

1. `valid=false` 时，保留刚才的过滤结果。
2. 没有变量项时，用 `compare_constants` 判断常量与 0 的比较；不成立则设为 bottom。
3. 只有一个变量，且系数为 `+1` 或 `-1` 时，用 `normalize_unary_guard` 整理条件，再收紧该变量。返回值中的 `variable`、`comparison`、`bound` 给出整理后的比较。
4. 对其他线性形式，保留操作数过滤的结果，本实验不要求进一步求解。

`normalize_unary_guard` 对不支持的形式返回 `valid=false`。它已经处理了系数符号和常量移项；你需要实现整理后的比较对区间的过滤。普通的 `x < y` 仍然需要在前面的操作数过滤中处理。

## 4. 理解分析结果

### 4.1 widening 也会产生误报

`14_counting_loop.fdulang` 如下：

```text
fn main() {
    let i = 0;
    while (i < 5) {
        i = i + 1;
    }
    if (i != 5) { abort; }
    halt;
}
```

具体执行退出循环时，`i` 一定等于 5，`abort` 不可达。

默认分析策略会把循环头的 `i` 扩大到 `[0, M]`。退出循环需要满足 `i >= 5`，因此循环出口得到 `[5, M]`，仍包含大于 5 的值，分析器就会报告 `A1`。

框架没有在 widening 之后执行 narrowing（重新收紧状态的迭代），因此这条告警会保留在参考结果中。调整 `--widen-delay` 会影响精度；与参考结果比较时使用默认值 2。

### 4.2 过程内分析怎样处理函数

Lab1 从 `main` 沿静态调用图选取需要分析的函数，再分别分析每个函数。即使调用写在不可达的分支里，它也会出现在静态调用图中。

被选中函数的参数从 top 开始分析；调用者把返回值目标设为 top，其他局部变量保持原状态，并继续分析调用之后的代码。本实验不把调用者的实参传给被调函数，也不根据被调函数是否返回来剪去调用后的路径。

例如，`21_call_parameter_safe.fdulang`：

```text
fn probe(v) {
    if (v != 0) { abort; }
    return 0;
}
fn main() {
    probe(0);
    halt;
}
```

实际调用中 `v` 总是 0，但过程内分析把 `probe` 的参数视为任意整数，因此报告 `A1`。Lab3 会通过传递实参和区分调用上下文改善这种情况。

前面提到的变量关系则留给 Lab2：即使 `y = x + 3`，分别记录 `x`、`y` 的区间也会丢失两者相差 3 的关系。八边形域能够保存这类约束。

## 5. 自测与完成要求

### 5.1 构建和单元测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
./build/fdulang_analysis_tests
```

`fdulang_analysis_tests` 检查区间操作及过程内求解器。可以列出测试名称，或按名称子串筛选：

```sh
./build/fdulang_analysis_tests --list
./build/fdulang_analysis_tests domain/join
./build/fdulang_analysis_tests domain/widen
```

部分测试通过 `assign` 和 `join` 构造初始状态，因此一个失败可能来自多个尚未完成的操作。四项任务完成后，应通过这个测试程序中的全部检查。

语言前端和解释器的测试位于 `tests/language/`；本实验的分析测试位于 `tests/labs/`。Lab1 的成绩只统计后者的分析测例。

### 5.2 运行整组分析测例

使用刚构建的分析器运行公共测例，并与参考告警集合比较：

```sh
python3 tools/evaluate_labs.py \
  --binary build/fdulang \
  --domain interval --mode intra \
  --expected tests/labs/expected/interval-intra.json \
  --json-out build/lab1-results.json \
  --csv-out build/lab1-results.csv
```

公共测例清单位于 `tests/labs/common.json`，共 28 个程序、42 处 `abort`。每处 `abort` 都有 ground truth，表示是否存在某个输入使程序执行到它。参考告警记录在 `tests/labs/expected/interval-intra.json`。

评价以每一处 `abort` 为单位：

| 统计量 | 含义 |
| --- | --- |
| TP | 实际可达，分析报告 |
| FP | 实际不可达，分析报告，即误报 |
| TN | 实际不可达，分析未报告 |
| FN | 实际可达，分析未报告，即漏报 |

准确率为 `(TP + TN) / (TP + FP + TN + FN)`，误报率为 `FP / (FP + TN)`，漏报率为 `FN / (FN + TP)`。

当前参考实现使用区间域、过程内模式和默认 `widen_delay=2`，结果如下：

| TP | FP | TN | FN | 准确率 | 误报率 | 漏报率 |
| --- | --- | --- | --- | --- | --- | --- |
| 15 | 8 | 19 | 0 | 80.95% | 29.63% | 0% |

参考实现也有误报。前文中的 `11_join_hole`、`14_counting_loop`、`21_call_parameter_safe` 就属于这种情况。

自测时检查两项结果：与 ground truth 比较应当没有漏报；与参考结果比较，每个程序报告的 `abort` 集合应当一致。仅有相同准确率，可能掩盖“少报一处、多报另一处”的错误。

项目也提供了包含构建的一键入口：

```sh
python3 tools/run_lab_tests.py --domain interval
```

这个入口先运行 Lab1 的区间域与过程内分析检查，再评价公共测例。加上 `--all-checks` 可以运行三个 Lab 的全部实现检查。

### 5.3 完成检查

- 完成四个 TODO，保持原有接口。
- 通过 `fdulang_analysis_tests` 的全部检查。
- 在默认配置下跑完公共测例，无漏报，逐例告警集合与参考结果一致。

对于出现的误报，你应当能结合 CFG 和区间状态说明原因：是路径合并填补了取值之间的空隙、变量关系没有被保存、widening 放宽了边界，还是过程内分析丢失了调用信息。
