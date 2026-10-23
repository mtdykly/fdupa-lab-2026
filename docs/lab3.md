# Lab3：上下文敏感的过程间分析

前两个 Lab 对选中的函数分别进行分析，把参数和调用返回值视为 top。本实验把函数之间的调用连接起来：将实参传给被调函数，再将正常返回值传回调用者。同时，用最近 k 个调用点区分不同的调用上下文。

你需要完成 `src/labs/lab3_context.cpp` 中的四项任务：

| 标记 | 函数 | 作用 |
| --- | --- | --- |
| `Lab3.1` | `next_context` | 生成被调函数的调用上下文 |
| `Lab3.2` | `bind_call_arguments` | 将实参绑定到形参，保留可表达的关系 |
| `Lab3.3` | `join_call_entry` | 合并同一上下文收到的入口状态 |
| `Lab3.4` | `join_return_summary` | 合并同一上下文的正常返回值 |

上下文存储、工作队列、返回通知以及递归中的 widening 都已经提供。这四项任务通过 `AbstractState` 接口操作状态，同一份实现用于区间域和八边形域。初始代码仍能运行分析，但会合并同一函数的所有调用上下文，并把参数和返回值近似为 top。

完成实验后，你应当能解释一次调用如何传递状态、k 如何影响分析精度，以及递归函数如何通过不动点迭代得到返回结果。

本实验使用已经完成的 [Lab1](lab1.md) 和 [Lab2](lab2.md)。安装与基本用法见[项目说明](../README.md)。本文命令以 Linux 为例，从含有 `CMakeLists.txt` 的项目根目录执行。

## 1. 把调用者的信息传进去

先回到公共测例 `tests/labs/cases/common/21_call_parameter_safe.fdulang`：

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

过程内分析单独处理 `probe`，入口参数 `v` 为 top，所以会报告 `A1`。但从 `main` 看，这次调用的实参就是 0。

过程间分析会得到以下状态：

| 位置 | 状态 |
| --- | --- |
| 调用 `probe(0)` | 实参为 `[0, 0]` |
| `probe` 入口 | 形参 `v: [0, 0]` |
| `v != 0` 的真分支 | bottom |
| `return 0` | 正常返回值为 `[0, 0]` |

因此，该程序的过程间分析结果为空告警集合。

可以先使用 `k=0` 观察参数传播的效果：

```sh
./build/fdulang analyze tests/labs/cases/common/21_call_parameter_safe.fdulang --domain interval --mode intra
./build/fdulang analyze tests/labs/cases/common/21_call_parameter_safe.fdulang --domain interval --mode inter --k 0
```

`k=0` 仍然会传递参数、收集返回值，只是同一函数的所有调用共用一个上下文。它与前两个 Lab 的过程内模式不同。

## 2. 为什么需要调用上下文

### 2.1 保留最近 k 个调用点

同一函数可能从不同位置、带着不同参数被调用。如果把这些入口状态全部合并，仍然会丢失信息。

本实验使用**调用点串（call string）**表示上下文。调用点是源码中一次函数调用的位置，CFG 中用全程序唯一的 `call_id` 标识。即使调用的是同一个函数，两个源码位置也有不同编号。

调用串按从旧到新的顺序记录调用点。例如：

```text
调用者的串：[C1, C4]
当前调用点：C7
k = 2
被调函数的串：[C4, C7]
```

只有最近两个调用点被保留。`k=1` 时结果为 `[C7]`，`k=0` 时结果为空串；调用链不足 k 层时，保留实际长度。

一个函数上下文的完整标识是：

```text
(函数, 调用点串)
```

只有函数和调用串都相同，才共用该上下文的入口状态、CFG 状态和返回摘要。调用串记录的是静态调用位置，不是参数值或动态调用次数。

### 2.2 一层与两层上下文的区别

看扩展测例 `tests/labs/cases/interprocedural/32_context_two_layers_safe.fdulang`：

```text
fn probe(value) {
    if (value == 1) { abort; }
    return 0;
}

fn wrap(value) {
    return probe(value);
}

fn main() {
    wrap(0);
    wrap(2);
    halt;
}
```

实际执行中，`probe` 只会收到 0 或 2，`abort` 不可达。这个程序的 CFG 中有三个调用点：

| 调用点 | 源码位置 |
| --- | --- |
| `C1` | `wrap` 中的 `probe(value)` |
| `C2` | `main` 中的 `wrap(0)` |
| `C3` | `main` 中的 `wrap(2)` |

不同 k 下，`probe` 的分析状态如下：

| k | 来自 `wrap(0)` 的上下文 | 来自 `wrap(2)` 的上下文 | `probe` 的参数状态 | 告警 |
| --- | --- | --- | --- | --- |
| 0 | `[]` | `[]` | 合并为 `[0, 2]` | `{A1}` |
| 1 | `[C1]` | `[C1]` | 合并为 `[0, 2]` | `{A1}` |
| 2 | `[C2, C1]` | `[C3, C1]` | 分别为 `[0, 0]`、`[2, 2]` | 空集 |

`k=1` 已经区分了 `wrap` 的两次外层调用，但到达 `probe` 时，最近的调用点都是 `C1`，于是又合并到一起。`k=2` 保留了 `C2`、`C3` 的区别，才避免这次合并。

区间域和八边形域在本例中的结果相同。对只有一个参数的 `probe`，把 0 和 2 合并后，两种域都会包含 1。

```sh
./build/fdulang dump-cfg tests/labs/cases/interprocedural/32_context_two_layers_safe.fdulang
./build/fdulang analyze tests/labs/cases/interprocedural/32_context_two_layers_safe.fdulang --domain interval --mode inter --k 1
./build/fdulang analyze tests/labs/cases/interprocedural/32_context_two_layers_safe.fdulang --domain interval --mode inter --k 2
```

如果 `main` 直接在两个位置调用 `probe(0)` 和 `probe(2)`，`k=1` 就能区分。这是 `30_context_same_callee_safe` 展示的情况。

## 3. 框架如何处理调用与返回

### 3.1 每个上下文保存什么

一个上下文包含该函数各 CFG 节点的入口状态，以及一个**返回摘要**。返回摘要表示这个上下文中所有已发现的正常返回值。

| 状态 | 槽位空间 | 初始值 |
| --- | --- | --- |
| CFG 节点状态 | 该函数的全部槽位，包括形参、局部变量和临时变量 | bottom |
| 返回摘要 | 只有一个槽位，表示返回整数 | bottom |

分析从 `main` 的空上下文开始，它的函数入口设为 top。其他函数不会预先从 top 分析；处理到状态非 bottom 的 Call 时，框架才创建或复用相应上下文。

因此，过程间的状态可以按 `(函数, 调用串, CFG 节点)` 区分。同一处 `abort` 只要在任一上下文中被分析为可达，就加入最终告警集合；重复告警会去重。

### 3.2 一次调用的传播过程

框架按以下过程处理调用：

1. 根据调用者的串和当前调用点，计算被调上下文。
2. 登记这个调用点对被调函数返回摘要的依赖。
3. 将实参绑定成被调函数的入口状态，与该上下文已有入口合并。
4. 被调函数分析到正常 Return 时，提取返回值并合并到摘要。
5. 摘要增长后，把依赖它的调用点重新放入工作队列，继续计算调用之后的状态。

重新处理调用点时，框架读取最新的调用前状态和返回摘要。如果调用发生时已经有非 bottom 摘要，也会立即利用它传播后继。

返回摘要仍为 bottom 时，调用后的代码暂不传播。这表示当前还没有发现正常返回；之后发现返回路径，通知机制会继续推进分析。`halt` 和 `abort` 终止整个具体程序，不提供正常返回值。函数末尾的隐式 `return 0` 则与显式 Return 一样处理。

返回通知保存了完整的调用者上下文及 Call 节点。截断后的调用串已经丢失较早的调用点，不能靠弹出最后一项恢复调用者；这部分依赖管理由框架完成。

### 3.3 提供的接口

Lab3 接口声明位于 `include/fdulang/lab3.hpp`，过程间求解器位于 `src/interprocedural.cpp`。调用与返回使用以下已有接口：

| 接口或字段 | 含义 |
| --- | --- |
| `CallString` | 从旧到新的调用点编号序列 |
| `call.arguments` | 已经求值的实参操作数，按参数顺序排列 |
| `callee.parameters` | 每个形参在被调函数中的槽位编号 |
| `callee.variables.size()` | 被调函数的总槽位数 |
| `state.remap(values)` | 从同一个旧状态并行提取操作数，生成新的状态 |
| `state.embed(dimensions, destinations)` | 将状态中的槽位映射到新槽位空间，其余位置设为 top |
| `state.clone()` | 复制状态 |
| `state.join_with(other)` | 使用当前数值域的 join 合并状态 |
| `state.assign_return(target, summary)` | 把一槽返回摘要写入调用者的目标，保留其他局部变量的信息 |
| `domain.top(n)`、`domain.bottom(n)` | 创建所选数值域的 n 槽 top 或 bottom |

`remap`、`embed` 和 `assign_return` 已分别为两个数值域实现。学生任务通过公共接口使用它们，无需转换为 `IntervalState` 或 `OctagonState`。

## 4. 实现四个操作

与前两个 Lab 的原地操作不同，Lab3 的接口返回**新的调用串或状态**。传入的 `const` 参数应保持不变；三个返回状态的接口使用 `std::unique_ptr<AbstractState>`。

### 4.1 更新上下文：next_context

`next_context(caller, call_site, k)` 返回被调函数的调用串：

- 在旧串末尾追加当前调用点。
- 保留最后 k 项，顺序从旧到新。
- `k=0` 时返回空串；长度不足 k 时保持实际长度。
- 调用者的原串保持不变。

例如：

| `caller` | `call_site` | k | 返回值 |
| --- | --- | --- | --- |
| `[]` | `C3` | 2 | `[C3]` |
| `[C1]` | `C3` | 2 | `[C1, C3]` |
| `[C1, C2]` | `C3` | 2 | `[C2, C3]` |
| `[C1, C2]` | `C3` | 0 | `[]` |

函数编号已经由框架作为上下文标识的另一部分保存，本操作只负责调用点串。

### 4.2 实参绑定：bind_call_arguments

`bind_call_arguments(caller, call, callee, domain)` 需要返回被调函数的入口状态，维数为 `callee.variables.size()`。

所有实参都从同一个旧调用者状态读取。`call.arguments` 中的复合表达式已经由此前的 CFG 指令计算完毕，此处只需处理常量或槽位操作数。

例如调用 `pair(y, x)`，假设调用前 `x=1`、`y=2`，则两个形参应分别收到 2 和 1。形参属于被调函数的新槽位空间，不能把它们当成调用者的槽位依次覆盖。

已有接口把绑定分成两个步骤：

1. `remap` 按实参顺序产生中间状态：第 i 槽表示第 i 个实参。
2. `embed` 将中间状态第 i 槽放到 `callee.parameters[i]`，其余被调局部槽位设为 top。

例如两个形参的槽位编号为 `[3, 1]`，中间状态中的第 0、1 槽就应分别映射到被调函数的第 3、1 槽。使用 `callee.parameters` 提供的编号，不假设形参总是从 0 开始连续排列。这两个辅助操作都会保留 bottom。

**关系要一起传递。** 如果调用者知道 `x + y = 10`，调用 `check_pair(x, y)` 后，八边形域应在两个形参之间保留这个关系。分别读取 `x`、`y` 的区间再绑定，会丢掉 Lab2 得到的信息。

`35_parameter_sum_safe` 就用于检查这一点：`main` 计算 `y = 10 - x`，再由 `check_pair` 判断 `x + y != 10`。区间域在 k=0、1、2 下均有误报，八边形域均能消除。

重复实参也应按同样方式处理。`pair(x, x)` 的两个形参在八边形域中相等；区间域则保留它们各自的范围。绑定过程应保持调用者状态不变。

### 4.3 合并函数入口：join_call_entry

`join_call_entry(current, incoming, domain, dimensions)` 的两个输入都位于同一被调函数的槽位空间中：

- `current` 是这个上下文已有的入口状态。
- `incoming` 是本次实参绑定得到的入口状态。
- `dimensions` 是被调函数的总槽位数。

返回值应为所选数值域中两者的 join。使用状态复制和公共 join 接口即可，输入保持不变。

例如一个上下文先收到 `value=0`，之后收到 `value=2`，入口需要覆盖两次调用，参数范围成为 `[0, 2]`。直接用新输入替换已有入口，会丢掉先前调用的信息。

bottom 是 join 的单位元。原入口为 bottom 时，结果就是新传入的状态；两个输入都为 bottom 时，结果仍为 bottom。

这个操作只负责合并。状态是否增长、是否重新入队以及是否使用 widening，由框架处理。

### 4.4 合并返回摘要：join_return_summary

`join_return_summary(current, incoming, domain)` 的两个输入都是**一槽状态**：

- `current` 保存该上下文已有的正常返回结果。
- `incoming` 保存新到达的 Return 所产生的结果。

框架已经把返回表达式投影到这一槽中。学生需要返回两者的 join，保持输入不变，bottom 的处理与入口合并相同。

多个 Return 可能返回不同整数。看 `39_multiple_returns_reachable.fdulang`：

```text
fn choose(value) {
    if (value < 0) { return 2; }
    return 4;
}

fn main() {
    let value = 0;
    input(value);
    if (value < -2) { halt; }
    if (value > 2) { halt; }
    let result = choose(value);
    if (result == 3) { abort; }
    if (result == 4) { abort; }
    halt;
}
```

`choose` 实际返回的集合是 `{2, 4}`。两个正常返回点合并后，区间域和八边形域的一槽摘要都会包含 `[2, 4]`，于是：

| abort | 实际情况 | 分析结果 |
| --- | --- | --- |
| `A1`：`result == 3` | 不可达 | 误报 |
| `A2`：`result == 4` | 可达，例如输入 0 | 报告 |

这六种过程间配置都报告 `{A1, A2}`。本例只有一处调用，增加 k 无法分开同一上下文中的两个返回分支。

摘要增长也可能影响较早的调用。例如 k=0 时，同一个函数的后一次调用带来新的返回值，框架需要重新处理此前依赖该摘要的调用点。你实现的合并操作应当保留旧结果与新结果，通知工作由框架完成。

## 5. 递归与精度边界

### 5.1 递归通过工作队列求解

`42_direct_recursion_safe.fdulang` 如下：

```text
fn countdown(value) {
    if (value <= 0) { return 0; }
    return countdown(value - 1);
}

fn main() {
    let result = countdown(3);
    if (result != 0) { abort; }
    halt;
}
```

具体执行会递减参数，最终返回 0。抽象分析中，递归调用同样经过实参绑定、入口合并和返回摘要传播：

1. `main` 将 3 传入 `countdown`。
2. 递归调用把减一后的参数传到对应上下文。调用串超过 k 时，较早的调用点被截断，后续调用会复用已有上下文。
3. 基例 `return 0` 产生非 bottom 摘要。
4. 等待这个摘要的递归调用点重新入队，将返回结果继续向调用者传播，最终到达 `main`。

两个域在 k=0、1、2 下都能得到空告警集合。

k 限制的是上下文保留的信息，并不限制递归可以执行多少层。框架不会在递归深度达到 k 时停止，而是继续合并状态，直到工作队列稳定。

对于 `46_infinite_recursion` 中的 `spin() { return spin(); }`，没有能先产生正常返回值的基例。摘要保持 bottom，调用之后的 `abort` 不可达。

### 5.2 递归中的 widening

有限 k 使潜在上下文数量有限，但上下文中的数值状态仍可能反复增长。除了局部循环，框架还在递归函数每个上下文的**函数入口和返回摘要**处使用 widening。

入口和摘要都需要考虑：即使入口已经稳定，反复递归返回也可能使摘要的范围继续扩大。这里沿用前两个 Lab 的默认延迟 `widen_delay=2`，初次从 bottom 变为非 bottom 不计入延迟，之后允许两次增长使用 join，再使用 widening；不执行 narrowing。

学生的两个合并操作只计算 join。递归识别、widening 的位置和时机都已经实现。

### 5.3 哪些信息仍然会丢失

**有限上下文。** `47_recursive_context_safe` 从两处外层调用分别传入 `value=0` 和 `value=2`。经过递归后，较早的调用点被截断，这两类调用仍会合并。当前两个域在 k=0、1、2 下都保留 `A1` 误报。

**返回值与实参的关系。** 本实验的摘要只有一个返回槽，不保存返回值与原实参之间的关系。例如，`identity(v)` 返回 `v`，调用前 `x: [0, 10]`，执行 `r = identity(x)` 后可以得到 `r: [0, 10]`，但当前实现无法由此恢复 `r = x`。即使用八边形和 k=2，后面的 `r != x` 也可能产生误报。

传入参数之间的关系可以由 `remap`、`embed` 保留；跨越返回边界的关系受一槽摘要限制。返回时，框架只覆盖调用者的结果目标，其他局部变量以及它们之间的关系保持原状。

**数值域的近似。** Lab1、Lab2 中的合流空洞和 widening 误报仍可能存在。上下文划分与数值域分别解决不同的信息损失：前者区分调用，后者决定每个上下文里能表示哪些取值关系。

## 6. 自测与完成要求

### 6.1 构建和单元测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
./build/fdulang_interprocedural_tests
```

完整测试同时覆盖两个数值域、参数关系和调用返回接口，需要使用已经完成的 Lab1、Lab2。调试时可以按名称子串筛选：

```sh
./build/fdulang_interprocedural_tests --list
./build/fdulang_interprocedural_tests hooks/context
./build/fdulang_interprocedural_tests interval/
./build/fdulang_interprocedural_tests octagon/
```

`hooks/context` 只检查调用串更新。按域筛选便于先调试一个已完成的域；最终应通过全部检查，包括实参关系与返回接口的测试。

语言实现测试仍位于 `tests/language/`。本实验的实现检查和精度测例位于 `tests/labs/`。

### 6.2 运行过程间测例

Lab3 使用 `tests/labs/all.json`：保留前两个 Lab 的 28 个公共程序，增加 18 个过程间程序，共 46 个程序、66 处 `abort`。扩展程序位于 `tests/labs/cases/interprocedural/`。

先运行一个配置，例如区间域、k=2：

```sh
python3 tools/evaluate_labs.py \
  --binary build/fdulang \
  --domain interval --mode inter --k 2 \
  --expected tests/labs/expected/interval-inter-k2.json \
  --json-out build/lab3-interval-k2.json \
  --csv-out build/lab3-interval-k2.csv
```

`--mode inter` 默认使用 `all.json`，`--k` 默认是 1。切换数值域或 k 时，同时选择相应的参考文件；六组文件按 `<domain>-inter-k<k>.json` 命名，其中域为 `interval` 或 `octagon`，k 为 0、1、2。

包含构建和实现检查的一键入口为：

```sh
python3 tools/run_lab_tests.py \
  --mode inter --domain all --k all \
  --json-out build/lab3-comparison.json \
  --csv-out build/lab3-comparison.csv
```

这里的 `--k all` 展开为 0、1、2，与两个域组合成六组配置。脚本先运行所选域及其过程间分析检查，再逐组评价。调试单个域时，可将 `--domain all` 改为 `interval` 或 `octagon`；加上 `--all-checks` 则运行三个 Lab 的全部实现检查。

### 6.3 在同一测例集上比较

比较过程内与过程间分析时，都使用 `all.json`。例如运行区间域的过程内对照：

```sh
python3 tools/evaluate_labs.py \
  --binary build/fdulang \
  --domain interval --mode intra \
  --manifest tests/labs/all.json \
  --expected tests/labs/expected/interval-intra-all.json
```

八边形对照使用 `--domain octagon` 和 `octagon-intra-all.json`。

以下参考结果全部来自这 46 个程序、66 处 `abort`，使用默认 `widen_delay=2`：

| 分析域 | 模式 | TP | FP | TN | FN | 准确率 | 误报率 | 漏报率 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 区间 | 过程内 | 23 | 24 | 19 | 0 | 63.64% | 55.81% | 0% |
| 八边形 | 过程内 | 23 | 22 | 21 | 0 | 66.67% | 51.16% | 0% |
| 区间 | 过程间 k=0 | 23 | 10 | 33 | 0 | 84.85% | 23.26% | 0% |
| 区间 | 过程间 k=1 | 23 | 9 | 34 | 0 | 86.36% | 20.93% | 0% |
| 区间 | 过程间 k=2 | 23 | 8 | 35 | 0 | 87.88% | 18.60% | 0% |
| 八边形 | 过程间 k=0 | 23 | 6 | 37 | 0 | 90.91% | 13.95% | 0% |
| 八边形 | 过程间 k=1 | 23 | 5 | 38 | 0 | 92.42% | 11.63% | 0% |
| 八边形 | 过程间 k=2 | 23 | 4 | 39 | 0 | 93.94% | 9.30% | 0% |

表中的过程内准确率与 Lab1、Lab2 文档不同，因为这里包含新增测例。原来的 `interval-intra.json`、`octagon-intra.json` 仍对应 28 个公共程序。

在这组测例中，参数与返回值传递减少了一批误报，k=1 和 k=2 又各消除一处上下文合并的误报。增加 k 通常需要维护更多上下文；具体精度还受到数值域、返回摘要和 widening 的影响。

### 6.4 完成检查

- 完成四个 TODO，保持公共接口和输入状态不变。
- 通过 `fdulang_interprocedural_tests` 的全部检查。
- 在区间、八边形与 k=0、1、2 的六组配置下跑完测例，无漏报，逐例告警集合与各自参考文件一致。
- 能解释两层调用例子中 k=1 与 k=2 的区别，以及递归返回值如何经摘要传播。
- 能说明关系参数为何需要整体传递，以及一槽返回摘要会丢失什么信息。

自测仍同时比较 ground truth 和参考告警集合。参考实现允许误报；完成标准是与指定配置的逐例结果一致，而不只是总准确率相同。
