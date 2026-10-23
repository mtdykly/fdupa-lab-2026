# FDULang 程序分析实验

本仓库用于程序分析课程的三个 Lab：区间分析、八边形域分析和过程间分析。实验目标是找出 FDULang 程序中可能执行到的 `abort` 语句，并比较不同分析方法的精度。

FDULang 是实验使用的一门简单命令式语言。词法和语法分析、控制流图（CFG）构造、解释器和分析工作队列由框架提供，学生需要完成的代码集中在 `src/labs/`。框架使用 C++17，测试脚本使用 Python，均只依赖标准库。

## 实验内容

| 实验 | 内容 | 任务文件 |
| --- | --- | --- |
| Lab1：过程内区间分析 | 用区间表示变量取值，实现赋值、条件过滤、join 和 widening。函数参数和调用返回值按任意整数处理。 | `src/labs/lab1_interval.cpp` |
| Lab2：过程内八边形域分析 | 记录 `±x ±y ≤ c` 形式的关系，利用变量间的约束减少误报。矩阵存储、闭包和 widening 已提供。 | `src/labs/lab2_octagon.cpp` |
| Lab3：过程间分析 | 传递实参和返回值，用最近 k 个静态调用点区分上下文；可以搭配前两个 Lab 的任一数值域。 | `src/labs/lab3_context.cpp` |

Lab1 和 Lab2 使用同一组测例。Lab3 增加调用和递归测例，用来比较过程内、过程间以及不同 k 的结果。三个 Lab 的任务和接口分别见 [Lab1 说明](docs/lab1.md)、[Lab2 说明](docs/lab2.md)、[Lab3 说明](docs/lab3.md)。

## 安装与构建

需要支持 C++17 的编译器、[CMake](https://cmake.org/download/) 3.16+ 和 [Python](https://www.python.org/downloads/) 3.9+。Python 用于运行测试脚本。

- **Linux（Ubuntu）**：`sudo apt install build-essential cmake python3`。
- **macOS**：`xcode-select --install` 安装 Clang，再安装 CMake 和 Python。
- **Windows**：安装 Visual Studio Build Tools 的“使用 C++ 的桌面开发”工作负载，以及 CMake、Python。编译器可用 MSVC，也可勾选 [C++ Clang tools for Windows](https://learn.microsoft.com/en-us/cpp/build/clang-support-msbuild) 使用 Clang。

克隆仓库或下载并解压源码后，在包含 `CMakeLists.txt` 的目录中执行以下命令。示例以 Linux 为准：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
./build/fdulang --help
```

macOS 使用相同命令。Windows 使用 Visual Studio 生成器时，构建命令加 `--config Debug`；选用 Clang 时，首次配置加 `-T ClangCL`。Windows 的运行路径为 `./build/Debug/fdulang.exe`，Python 命令用 `python`。

## FDULang

一个 `.fdulang` 文件就是一个完整程序，从无参数的 `main` 开始执行。例如，`examples/03_recursion.fdulang` 读取 n，递归计算 1 到 n 的和，结果大于 10 时执行 `abort`：

```text
fn main() {
    let n = 0;
    input(n);
    if (n < 0) { halt; }
    if (n > 10) { halt; }

    let result = sum_to(n);
    if (result > 10) { abort; }
    halt;
}

fn sum_to(n) {
    if (n == 0) {
        return 0;
    }
    return n + sum_to(n - 1);
}
```

`input(n)` 读取一个整数；`return` 返回当前函数，执行到函数末尾默认返回 0；`halt`、`abort` 分别正常、异常结束整个程序。

### 词法

源文件使用 UTF-8，文件开头可带 BOM。标识符和符号使用 ASCII，注释中可包含 Unicode 字符。词法规则用正则表达式表示：

```text
identifier      = [A-Za-z_][A-Za-z0-9_]*
decimal         = [0-9]+
invalid_decimal = [0-9]+[A-Za-z_][A-Za-z0-9_]*
symbol          = <=|>=|==|!=|[(){};,=+\-<>]
whitespace      = [ \t\r\n\f\v]+
line_comment    = //[^\r\n]*
block_comment   = /\*([^*]|\*+[^*/])*\*+/
```

扫描时采用最长匹配。`whitespace`、`line_comment` 和 `block_comment` 被跳过；`invalid_decimal` 对应词法错误，例如 `12abc`、`1_000`、`0x10`。其他无法匹配的字符也属于词法错误。

完整匹配 `identifier` 后，以下单词识别为关键字：

```text
fn  let  input  if  else  while  return  halt  abort
```

其余标识符区分大小写，`main` 是普通标识符。`decimal` 为十进制数字串，允许前导零；正负号分别识别为 `+`、`-`。块注释在第一个 `*/` 处结束。

### 上下文无关文法

下面用 EBNF 表示语法，起始符号为 `program`。双引号内为终结符，`identifier` 和 `decimal` 来自词法规则，`EOF` 表示文件结束。逗号表示连接，`|` 表示选择，`[...]` 表示可选，`{...}` 表示零次或多次重复，圆括号用于分组。

```ebnf
program        = function, { function }, EOF ;
function       = "fn", identifier, "(", [ parameters ], ")", block ;
parameters     = identifier, { ",", identifier } ;
block          = "{", { statement }, "}" ;

statement      = declaration | assignment | call_statement | input_statement
               | if_statement | while_statement | return_statement
               | "halt", ";" | "abort", ";" | block | ";" ;
declaration    = "let", identifier, "=", expression, ";" ;
assignment     = identifier, "=", expression, ";" ;
call_statement = call, ";" ;
input_statement = "input", "(", identifier, ")", ";" ;
if_statement   = "if", "(", expression, ")", block,
                 [ "else", ( block | if_statement ) ] ;
while_statement = "while", "(", expression, ")", block ;
return_statement = "return", expression, ";" ;

expression     = sum, [ comparison, sum ] ;
comparison     = "<" | "<=" | ">" | ">=" | "==" | "!=" ;
sum            = unary, { ( "+" | "-" ), unary } ;
unary          = ( "+" | "-" ), unary | primary ;
primary        = decimal | call | identifier | "(", expression, ")" ;
call           = identifier, "(", [ arguments ], ")" ;
arguments      = expression, { ",", expression } ;
```

一元 `+ -` 右结合，二元 `+ -` 左结合，比较运算符优先级最低且不可连写。

静态检查将表达式分为整数和条件两类。比较的操作数为整数，结果为条件；加减的操作数和结果均为整数。`if`、`while` 接受条件，声明、赋值、实参和 `return` 接受整数。括号保留表达式类别，因此 `if ((x < y))` 合法，`if (x)` 和 `let b = x < y;` 是静态错误。

变量使用有符号 32 位整数，加减溢出产生运行时错误。局部变量在所在块中可见，使用前必须声明，同一函数内的参数和局部变量不能重名。数值范围、名称和作用域的完整规则见[语言规范](docs/language.md)，独立文法文件为 [grammar.ebnf](docs/grammar.ebnf)。

## 运行与分析

先检查上面的程序，再执行：

```sh
./build/fdulang check examples/03_recursion.fdulang
./build/fdulang run examples/03_recursion.fdulang
```

`check` 检查语言规则并构造 CFG。`run` 执行到 `input` 时等待输入。输入 `4` 并回车，结果是 `normal`；重新运行并输入 `5`，结果是 `abort A1`，后面附有源码位置。FDULang 没有打印语句，运行状态由解释器输出。

也可以在 `build/input.txt` 中写入一行 `5`，再运行：

```sh
./build/fdulang run examples/03_recursion.fdulang --input build/input.txt
```

多个输入用空白分隔。非法整数、输入不足或算术溢出会产生 `runtime_error`。

`run` 得到某一次输入下的执行结果；`analyze` 通过抽象状态计算可能到达的 `abort`：

```sh
./build/fdulang analyze examples/02_relation.fdulang --domain interval --mode intra
./build/fdulang analyze examples/02_relation.fdulang --domain octagon --mode intra
./build/fdulang analyze examples/03_recursion.fdulang --domain interval --mode inter --k 1
```

`02_relation.fdulang` 限制 x 在 `[0,10]`，令 `y = 10 - x`，然后在 `x + y != 10` 时执行 `abort`。完成实验后，区间分析仍会报告这个不可达的 A1；八边形分析可以保留 `x + y = 10`，从而排除它。

`--domain` 选择区间域或八边形域，`--mode` 选择过程内或过程间分析。过程间的 `--k` 表示保留的调用点串长度，默认 1，取 0 时不区分上下文。`check`、`run` 和 `analyze` 都可加 `--json` 输出 JSON。

`A1`、`A2` 等编号按源文件中 `abort` 的位置排列。同一语句在循环或递归中执行多次，仍使用同一个编号。更多选项和退出码见[工具说明](docs/tooling.md)。

### 查看 CFG

源程序经过解析和静态检查后转换成 CFG，解释器和分析器都在 CFG 上工作：

```sh
./build/fdulang dump-cfg examples/02_relation.fdulang
```

每个函数有自己的变量表和节点表，复杂表达式会拆成简单指令。

## 测试

语言实现测试位于 `tests/language/`，检查解析器、CFG 和解释器，不参与 Lab 的精度评价。脚本会自动配置、构建并运行测试：

```sh
python3 tools/run_language_tests.py
```

Lab 测试位于 `tests/labs/`。一键入口会构建代码，检查域操作和求解器，再运行正式测例、统计精度并与参考告警集合比较：

```sh
python3 tools/run_lab_tests.py                         # Lab1
python3 tools/run_lab_tests.py --domain octagon        # Lab2
python3 tools/run_lab_tests.py --domain all            # 对比两个域
python3 tools/run_lab_tests.py --mode inter --domain all --k all  # Lab3 的六种配置
```

过程内默认使用 `common.json` 中的 28 个程序，过程间默认使用 `all.json` 中的 46 个程序。

一键入口按 `--domain` 和 `--mode` 运行相关的实现检查，通过后评价正式测例。加上 `--all-checks` 会运行三个 Lab 的全部实现检查。也可以直接评价已构建的程序：

```sh
python3 tools/evaluate_labs.py --binary build/fdulang --domain interval --expected tests/labs/expected/interval-intra.json --json-out build/lab1-results.json --csv-out build/lab1-results.csv
```

评价器使用已有的可执行文件。`--expected` 指定与域、分析模式和 k 对应的参考告警集合；省略时只比较 ground truth。

评价以每条静态 abort 为单位，逐例比较参考告警集合，并统计准确率、误报率和漏报率。上近似分析允许误报，要求无漏报。指标定义和结果格式见[分析测例说明](tests/labs/README.md)。

## 代码结构

```text
include/fdulang/     AST、CFG、运行时和分析域的接口
src/                前端、CFG、解释器、求解器与命令行
  domains/          数值域辅助实现
  labs/             三个 Lab 的任务文件
docs/               语言规范、工具说明和 Lab 文档
examples/           语言示例
tests/language/     语言实现测试
tests/labs/         分析实现检查、测例、真值和预期结果
tools/              构建、测试和评价脚本
```

| 源码 | 职责 |
| --- | --- |
| `src/frontend.cpp` | 词法分析、递归下降解析、名称解析和静态检查 |
| `src/ir.cpp` | AST 转 CFG、临时变量分配、CFG 验证和打印 |
| `src/runtime.cpp` | 解释执行 CFG，处理输入、调用栈和程序结束 |
| `src/analysis.cpp` | 过程内工作队列、状态传播和 widening 调度 |
| `src/interprocedural.cpp` | 过程间上下文、调用和返回依赖、递归分析 |
| `src/domains/interval_support.cpp` | 区间表示、边界运算、条件整理和变量映射 |
| `src/domains/octagon_support.cpp` | 八边形矩阵、闭包、widening 和变量映射 |
| `src/labs/lab1_interval.cpp`、`lab2_octagon.cpp`、`lab3_context.cpp` | 三个 Lab 待完成的操作 |
| `src/main.cpp` | 命令行选项和文本、JSON 输出 |
| `CMakeLists.txt` | 构建目标和测试入口 |
