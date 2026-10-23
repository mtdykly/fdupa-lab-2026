# Lab2：过程内八边形域分析

Lab1 为每个变量保存一个区间。本实验进一步记录变量之间的加减关系，例如 `x - y <= 3`、`x + y == 10`，用这些信息减少误报。

你需要完成 `src/labs/lab2_octagon.cpp` 中的五项任务：

| 标记 | 函数 | 作用 |
| --- | --- | --- |
| `LAB2-CONSTRAIN` | `constrain` | 将八边形约束编码到矩阵中 |
| `LAB2-FORGET` | `forget` | 遗忘一个变量，保留其他变量的信息 |
| `LAB2-JOIN` | `join_with` | 合并来自不同路径的状态 |
| `LAB2-ASSIGN` | `assign` | 更新赋值目标及相关约束 |
| `LAB2-ASSUME` | `assume` | 根据分支条件添加约束 |

矩阵存储、闭包、widening 和 CFG 求解器已经提供。五项任务都修改当前 `OctagonState`，初始代码可以运行分析，但会丢失大量关系。Lab2 使用独立任务文件，不需要改动 Lab1 的实现。

完成实验后，你应当能把一个加减约束写成矩阵项，说明赋值、合流和遗忘对变量关系的影响，并解释同一程序在区间域和八边形域下为何会得到不同结果。

状态传播、top/bottom 和过程内调用的约定沿用 [Lab1](lab1.md)。安装与基本用法见[项目说明](../README.md)。本文命令以 Linux 为例，从含有 `CMakeLists.txt` 的项目根目录执行。

## 1. 区间丢失了什么

看公共测例 `tests/labs/cases/common/17_relation_difference.fdulang`：

```text
fn main() {
    let x = 0;
    input(x);
    if (x < 0) { halt; }
    if (x > 10) { halt; }
    let y = x + 3;
    if (y - x != 3) { abort; }
    halt;
}
```

前两个分支把 `x` 限制在 `[0, 10]`。执行 `y = x + 3` 后，区间分析得到：

```text
x: [0, 10]
y: [3, 13]
```

分别记录这两个区间，会把 `x = 0, y = 13` 等不可能出现的组合也包含进来。按区间减法计算，`y - x` 的结果为 `[-7, 13]`，所以 Lab1 会报告 `A1`。

八边形域可以同时记录：

```text
0 <= x <= 10
y - x <= 3
x - y <= -3
```

后两个不等式合起来就是 `y - x = 3`，因此 `y - x != 3` 不可能成立。Lab2 应当得到空告警集合。

构建后可以对同一文件分别运行两个域：

```sh
./build/fdulang analyze tests/labs/cases/common/17_relation_difference.fdulang --domain interval --mode intra
./build/fdulang analyze tests/labs/cases/common/17_relation_difference.fdulang --domain octagon --mode intra
```

这里仍然是过程内分析。改变的是状态表示和状态上的操作，CFG 与工作队列保持相同。

## 2. 用 DBM 表示八边形约束

### 2.1 从差约束到加减约束

Zone 域记录 `x - y <= c` 和一元上下界。八边形域允许两侧独立选择正负号，记录 `±x ±y <= c`，因此也能表示 `x + y <= c`。在二维情况下，这些约束的边界只有水平、竖直和两种对角线方向，围出的形状至多有八条边。这是“八边形”一名的来源。两者都可以用差分约束矩阵（Difference-Bound Matrix，DBM）表示。[The Octagon Abstract Domain](https://perso.lip6.fr/Antoine.Mine/publi/article-mine-ast01.pdf)

关键做法是：为每个 CFG 槽位 `x` 设置两个矩阵索引，分别表示数学上的 `+x` 和 `-x`。假设函数共有 `n` 个槽位，矩阵大小就是 `2n × 2n`。临时变量也计入这些槽位。

```text
signed(2*x)     = +x
signed(2*x + 1) = -x

D[p, q] = c  表示  signed(p) - signed(q) <= c
```

这里的负号是约束表示中的数学符号，不是在 FDULang 中执行一次取负指令。

例如，`x + y <= 5` 可以写成 `(+x) - (-y) <= 5`。常见约束对应的矩阵项如下：

| 约束 | 对应矩阵项的上界 |
| --- | --- |
| `x - y <= c` | `D[2*x, 2*y] <= c` |
| `x + y <= c` | `D[2*x, 2*y+1] <= c` |
| `-x - y <= c` | `D[2*x+1, 2*y] <= c` |
| `-x + y <= c` | `D[2*x+1, 2*y+1] <= c` |
| `x <= c` | `D[2*x, 2*x+1] <= 2*c` |
| `x >= c` | `D[2*x+1, 2*x] <= -2*c` |

一元约束中的系数 2 来自 `(+x) - (-x) = 2x`。等式需要写成两个方向的不等式，例如 `x + y = 10` 对应 `x + y <= 10` 和 `-x - y <= -10`。

### 2.2 同一约束的两个位置

`(+x) - (-y)` 和 `(+y) - (-x)` 都等于 `x + y`，所以矩阵里有两个位置表达同一个约束，它们应保持一致。

对索引 `p`，`p ^ 1` 表示切换正负号。矩阵需要满足：

```text
D[p, q] = D[q ^ 1, p ^ 1]
```

这称为 coherence。框架提供的 `add_difference(p, q, c)` 会同时收紧这两个位置。收紧时取已有上界与 `c` 的较小值：添加新约束相当于对状态取交。

矩阵在代码中存为一维数组 `dbm_`，`cell(p, q)` 给出对应下标，`width()` 返回 `2n`。每项类型为 `DbmBound`：

- `DbmBound::finite(c)` 表示有限上界 `c`。
- 默认构造的 `DbmBound{}` 表示正无穷，即这一项暂时不施加限制。
- `bound_min`、`bound_max`、`bound_leq` 已经处理了无穷的比较。

正常状态的对角线为 0。FDULang 的每个变量都在 `[m, M]` 内，其中 `m=-2147483648`、`M=2147483647`；top 也保留这些语言边界。bottom 表示没有可行取值，用 `bottom_` 单独记录。bottom 状态可能尚未分配矩阵，操作时应先处理这一情况。

### 2.3 闭包有什么用

矩阵既要保存直接加入的约束，也要反映它们能推导出的关系。例如：

```text
x - y <= 2
y - z <= 3
----------------
x - z <= 5
```

矩阵上的这一推导对应 `D[p, q] <= D[p, k] + D[k, q]`，可以用最短路径算法计算。利用正负槽位之间的联系，还能从一元界推导关系，例如 `x <= 2`、`y >= 1` 推出 `x - y <= 1`。

本实验使用整数，还需要做整数取整。比如 `2*x <= -1` 等价于 `x <= -1`；`-1 / 2` 必须向下取整，不能使用 C++ 向零截断的结果。约束 `x = y` 和 `x + y = 1` 虽然有有理数解，却没有整数解，也应当判为 bottom。

这些工作由 `normalize()` 完成：它恢复 int32 边界、传播约束、执行整数紧闭包，并检查矛盾。学生操作返回时，应保持状态已经规范化。整数闭包的算法背景可参考 [Incrementally Closing Octagons](https://arxiv.org/abs/1610.02952)；本实验直接使用提供的实现。

### 2.4 已有辅助操作

`OctagonState` 的声明位于 `include/fdulang/octagon.hpp`，辅助实现位于 `src/domains/octagon_support.cpp`。

| 操作 | 用途 |
| --- | --- |
| `add_difference(p, q, c)` | 收紧矩阵项及其 coherence 位置；之后仍需规范化 |
| `normalize()` | 计算整数紧闭包，恢复语言边界，检查矛盾 |
| `value(variable)`、`read(operand)` | 查询变量或操作数的区间范围 |
| `add_interval(variable, interval)` | 与给定区间取交，裁剪至 int32 范围并规范化 |
| `affine_image(target, source, sign, offset)` | 执行 `target = sign * source + offset`，其中 `sign` 为 `±1` |
| `assume_inequality(expression, op)` | 整理不等式条件，再调用 `constrain` |
| `compatible(other)` | 将另一个抽象状态作为相同维数的八边形状态访问 |
| `set_bottom()` | 将当前状态设为 bottom |
| `reset_widening()` | 清除当前状态上的 widening 历史 |

矩阵的规范化状态用于查询和传播；框架另外保存一份 widening 历史，记录循环中已经放弃的约束。五项任务中已有的 `reset_widening()` 调用应保留。widening 历史的维护和闭包算法都不属于待实现内容。

## 3. 实现五个操作

### 3.1 添加约束：constrain

接口 `constrain(x, sx, y, sy, c)` 表示向当前状态添加：

```text
sx * x + sy * y <= c
```

`x`、`y` 是槽位编号，`sx`、`sy` 为 `+1` 或 `-1`；`y=-1` 表示一元约束 `sx*x <= c`，此时 `sy` 不参与计算。

例如：

| 调用 | 含义 |
| --- | --- |
| `constrain(x, 1, y, -1, 3)` | `x - y <= 3` |
| `constrain(x, 1, y, 1, 10)` | `x + y <= 10` |
| `constrain(x, -1, -1, 1, -2)` | `x >= 2` |

根据第 2 节的索引约定，选出对应的矩阵位置，调用 `add_difference` 加入约束，再调用 `normalize`。新增约束只能收紧当前状态；原状态为 bottom 时仍为 bottom。

还需要处理以下情况：

- **两项是同一个变量，符号相反。** 左侧抵消为 0。`0 <= c` 成立时状态不变，否则设为 bottom。
- **两项是同一个变量，符号相同。** 得到 `2*x <= c` 或 `-2*x <= c`，由整数紧闭包处理奇数边界。
- **一元约束。** 矩阵中存入的是 `2*c`，而不是 `c`。

`c` 的类型是 `std::int64_t`。在计算 `2*c` 前，先用语言值域判断约束：若 `c` 小于左式的最小可能值，则没有解；若 `c` 不小于左式的最大可能值，则约束恒成立。`+x` 的范围为 `[m, M]`，`-x` 的范围为 `[-M, -m]`。这样也能处理接近 int64 边界的参数，剩余需要编码的常量都很小。

### 3.2 遗忘变量：forget

`forget(x)` 表示不再知道 `x` 的值。它用于输入、新赋值覆盖旧值，以及临时变量的清理。

遗忘 `x` 时，需要去掉涉及 `+x` 和 `-x` 的行、列上的旧约束，保留其他位置，再恢复对角线的 0 并规范化。规范化后，`x` 仍受到语言的 int32 范围限制。

这里不能把整个状态清空。例如，旧状态包含：

```text
a - x <= 1
x - b <= 2
```

闭包已经推导出 `a - b <= 3`。遗忘 `x` 后，这条只涉及 `a`、`b` 的约束仍应保留。因为输入状态已经完成闭包，删除 `x` 对应的行、列就能留下这些推导结果。

原状态为 bottom 时，遗忘后仍为 bottom。

### 3.3 合并状态：join_with

`join_with(other)` 要包含两条路径的所有可能取值，结果保存在当前状态中，`other` 保持不变。

对于两个已完成整数紧闭包的矩阵，逐项取较大的上界，再规范化：

```text
D_join[p, q] = max(D_left[p, q], D_right[p, q])
```

例如，一条路径有 `x - y <= 1`，另一条有 `x - y <= 3`，合并后只能保证 `x - y <= 3`。使用 `bound_max` 即可同时处理有限界和无穷。

bottom 的处理与 Lab1 相同：`S ⊔ bottom = S`、`bottom ⊔ S = S`。

关系域的 join 有时比区间 join 保留更多信息。假设两条路径分别到达：

```text
路径一：x = 0,  y = 10
路径二：x = 10, y = 0
```

区间合并得到 `x: [0, 10]`、`y: [0, 10]`。八边形合并除了这些范围，还保留两条路径都成立的 `x + y = 10`，因此不会包含 `x = 5, y = 4`。

它仍会包含中间的 `x = 5, y = 5`。八边形可以保留关系，但不能精确表示任意离散点集合。

### 3.4 赋值：assign

`assign(target, op, left, right)` 仍处理 `Copy`、`Neg`、`Add`、`Sub`。与 Lab1 一样，所有右值都从赋值前的状态读取。

本实验分两种情况处理。

**可以精确处理的单变量形式。** 当指令的操作数能直接组成 `target = ±source + c` 时，使用已有的 `affine_image`：

| 右侧形式 | `source` | `sign` | `offset` |
| --- | --- | --- | --- |
| `y` | `y` | `1` | `0` |
| `-y` | `y` | `-1` | `0` |
| `y + c` 或 `c + y` | `y` | `1` | `c` |
| `y - c` | `y` | `1` | `-c` |
| `c - y` | `y` | `-1` | `c` |

这里的 `c` 指 `Operand.is_constant` 为真的常量操作数。`affine_image` 会同时更新与目标有关的所有关系，处理旧目标值的覆盖和 int32 范围。

自赋值也交给这个辅助操作。例如原先 `x = y`，执行 `x = x + 1` 后，应得到 `x - y = 1`；执行 `x = -x` 则需要翻转相关约束。调用 `affine_image` 前应保留旧状态，不能先 `forget(target)`。

**常量计算和一般表达式。** 常量右值直接得到单点区间；其他不能按上表处理的加减法，使用旧操作数区间计算结果，规则与 Lab1 一致。两侧是同一槽位的 `x - x` 应得到 `[0, 0]`。

对于一般的 `z = x + y`，等式 `z - x - y = 0` 涉及三个变量，超出了八边形约束的形式。本实验用区间结果近似这类赋值：

1. 从旧状态读出所有需要的操作数，计算结果区间。
2. 遗忘目标的旧值和旧关系。
3. 用 `add_interval` 加入目标的新范围。

`add_interval` 的含义是取交，所以写入新值前需要遗忘旧目标。其他变量之间的关系应当保留。参考策略在这些一般表达式上使用区间运算，不额外求 DBM 中表达式的最紧界。

例如 `x: [0, 2]`、`y: [3, 4]` 时，`z = x + y` 得到 `z: [3, 6]`。即使 `x`、`y` 之间还有关系，这一步也按上述区间规则计算。

溢出语义与 Lab1 相同：传播能够正常完成指令的结果。`affine_image` 和 `add_interval` 会处理语言边界；区间回退的结果与 int32 范围没有交集时，状态成为 bottom。计算端点、常量偏移时使用 64 位整数；例如对 int32 最小值取负，应先转为 int64 再取负。

### 3.5 条件过滤：assume

`assume(branch, truth)` 根据所选分支添加约束。假分支先用 `negate_compare` 取得原比较的逻辑补集。所有过滤都从当前状态出发，保留满足条件的可能执行；原状态为 bottom 时仍为 bottom。

将一个比较的左式减右式记为 `E`，条件就变成了 `E op 0`。已有的 `assume_inequality(E, op)` 支持 `<`、`<=`、`>`、`>=`，它负责移项、严格整数边界以及调用 `constrain`。例如：

```text
x + y < 10   等价于   x + y <= 9
x - y > 3    等价于   -x + y <= -4
```

它能处理常量条件、单变量系数为 `±1` 或 `±2` 的条件，以及两个变量系数各为 `±1` 的条件。对于无效元数据或不支持的形式，它保持当前状态。

你需要组合这个辅助操作，实现另外两种比较：

| 比较 | 处理方式 |
| --- | --- |
| `E == 0` | 在同一状态上依次加入 `E <= 0` 和 `E >= 0` |
| `E != 0` | 从同一个原状态复制两份，分别加入 `E < 0`、`E > 0`，再 join |

对于 `!=`，两个条件代表两条可选路径。若已有 `x + y = 10`，那么 `x + y < 10` 和 `x + y > 10` 都会成为 bottom，合并后也是 bottom。若已有的只是 `x: [0, 10]`，过滤 `x != 5` 后，两支合并又会得到 `[0, 10]`；内部的空洞仍无法保留。

每个 Branch 要按顺序应用两次条件过滤：

1. 用 `branch.left - branch.right` 构造线性表达式，过滤已经求值完成的操作数。
2. 若状态仍非 bottom，再用 `branch.condition` 过滤原始源条件。

`LinearExpr` 用 `coefficients` 保存各槽位的系数，用 `constant` 保存常量项。构造第一步的表达式时，左操作数贡献 `+1`，右操作数贡献 `-1`，常量计入 `constant`。同一槽位的系数要合并，因此 `x - x` 的变量项会抵消。

第二步对本实验很重要。第 1 节的程序会先计算临时变量 `t = y - x`，再比较 `t != 3`。参考赋值策略对这次减法使用区间回退，`t` 不一定得到精确的 `[3, 3]`；但原状态仍保留 `y - x = 3`，而 `condition` 保存的是 `y - x - 3`。应用原始条件，就能判断真分支不可达。

这些条件在相关算术指令正常完成之后才使用；算术中的溢出路径已经由前面的赋值处理。

## 4. 对比精度

### 4.1 和关系

八边形域也能处理和关系。`19_relation_sum.fdulang` 如下：

```text
fn main() {
    let x = 0;
    input(x);
    if (x < 0) { halt; }
    if (x > 10) { halt; }
    let y = 10 - x;
    if (x + y != 10) { abort; }
    halt;
}
```

区间域分别得到 `x: [0, 10]`、`y: [0, 10]`，从而把 `x + y` 估计为 `[0, 20]`，报告 `A1`。八边形域通过赋值保存 `x + y = 10`，再利用分支的原始条件排除这条告警。

公共测例还包含 `18_relation_difference_reachable` 和 `20_relation_sum_reachable`。这两个程序的 `abort` 确实可达，两个域都应报告 `A1`。

### 4.2 仍然会有哪些误报

在当前参考实现中：

| 测例 | ground truth | 区间域告警 | 八边形域告警 |
| --- | --- | --- | --- |
| `17_relation_difference` | 空集 | `{A1}` | 空集 |
| `19_relation_sum` | 空集 | `{A1}` | 空集 |
| `11_join_hole` | 空集 | `{A1}` | `{A1}` |
| `14_counting_loop` | 空集 | `{A1}` | `{A1}` |
| `21_call_parameter_safe` | 空集 | `{A1}` | `{A1}` |

`11_join_hole` 的两条路径分别令 `x=0` 和 `x=2`。八边形合并仍会包含 `x=1`，所以关系域也会受到合流的精度损失影响。

`14_counting_loop` 的误报来自 widening。框架沿用 Lab1 的默认 `widen_delay=2`，不执行 narrowing；八边形域的 widening 也会放弃不稳定的约束。

`21_call_parameter_safe` 的误报来自过程内分析：函数参数从 top 开始，调用返回值也按 top 处理。八边形域可以保留调用者中未被覆盖变量之间的关系，但本实验仍不传递实参或返回关系。调用信息的利用留给 Lab3。

## 5. 自测与完成要求

### 5.1 构建和单元测试

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
./build/fdulang_octagon_tests
```

这个测试程序检查约束编码、闭包调用、遗忘、合并、赋值、条件过滤以及八边形域接入求解器后的行为。可以按名称子串筛选：

```sh
./build/fdulang_octagon_tests --list
./build/fdulang_octagon_tests encoding/
./build/fdulang_octagon_tests forget/
./build/fdulang_octagon_tests assignment/
```

部分测试通过 `constrain`、`assign` 构造状态，任务之间存在依赖。五项任务完成后，应通过这个测试程序中的全部检查。

Lab2 的八边形实现和单独自测不依赖 Lab1 四个 TODO 的完成情况；两域对比时，区间域应使用已经完成的 Lab1 实现。语言测试仍位于 `tests/language/`，分析实现检查和精度测例位于 `tests/labs/`。

### 5.2 运行公共测例

Lab1 和 Lab2 使用完全相同的 `tests/labs/common.json`，共 28 个程序、42 处 `abort`。

```sh
python3 tools/evaluate_labs.py \
  --binary build/fdulang \
  --domain octagon --mode intra \
  --expected tests/labs/expected/octagon-intra.json \
  --json-out build/lab2-results.json \
  --csv-out build/lab2-results.csv
```

再按 Lab1 的命令运行区间域，即可对照两份逐例结果。参考实现都使用过程内模式和默认 `widen_delay=2`：

| 分析域 | TP | FP | TN | FN | 准确率 | 误报率 | 漏报率 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 区间 | 15 | 8 | 19 | 0 | 80.95% | 29.63% | 0% |
| 八边形 | 15 | 6 | 21 | 0 | 85.71% | 22.22% | 0% |

统计口径与 Lab1 相同。八边形域在这组测例中消除了差关系、和关系各一处误报，真实可达的 `abort` 均被保留。

项目的一键入口包含构建和分析检查：

```sh
python3 tools/run_lab_tests.py --domain octagon
python3 tools/run_lab_tests.py --domain all
```

`--domain octagon` 先运行 Lab2 的八边形域与过程内分析检查，再评价公共测例。`--domain all` 检查两个域，并在同一清单上评价和对比。加上 `--all-checks` 可以运行三个 Lab 的全部实现检查。

### 5.3 完成检查

- 完成五个 TODO，保持原有接口。
- 通过 `fdulang_octagon_tests` 的全部检查。
- 在默认配置下跑完公共测例，无漏报，逐例告警集合与 `octagon-intra.json` 一致。
- 能解释 `17_relation_difference`、`19_relation_sum` 中保留了哪些关系，以及为什么 `11_join_hole` 仍有误报。

与 Lab1 一样，自测同时比较 ground truth 和参考告警集合；总准确率相同不能代替逐例结果一致。
