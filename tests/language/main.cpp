#include "fdulang/frontend.hpp"
#include "fdulang/ir.hpp"
#include "fdulang/runtime.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace fdulang;

struct Test {
    std::string name;
    std::function<void()> run;
};

void require(bool condition, const std::string& explanation) {
    if (!condition) throw std::runtime_error(explanation);
}

std::string body(const std::string& text) {
    return "fn main() { " + text + " }";
}

ProgramCfg compile(const std::string& source) {
    AstProgram ast = parse_and_check("language-test.fdulang", source);
    return lower_to_cfg(ast);
}

ExecutionResult run(const std::string& source, const std::string& input = "",
                    const ExecutionOptions& options = ExecutionOptions{}) {
    ProgramCfg cfg = compile(source);
    std::istringstream stream(input);
    return execute(cfg, stream, options);
}

void expect_status(const ExecutionResult& result, ExecutionStatus expected,
                   int abort_id = 0) {
    require(result.status == expected,
            std::string("expected ") + status_name(expected) + ", got " +
            status_name(result.status) + " (" + result.code + ": " + result.message + ")");
    if (expected == ExecutionStatus::Abort) {
        require(result.abort_id == abort_id, "wrong static abort ID");
    } else {
        require(result.abort_id == 0, "non-abort result contains an abort ID");
    }
}

void expect_error(const std::string& source, const std::string& category) {
    try {
        (void)parse_and_check("invalid.fdulang", source);
    } catch (const Diagnostic& error) {
        require(error.category == category,
                "expected " + category + ", got " + error.category + ": " + error.what());
        require(error.span.begin.line >= 1 && error.span.begin.column >= 1,
                "diagnostic position is not one based");
        return;
    }
    throw std::runtime_error("source was accepted; expected " + category);
}

const Token& token_of(const std::vector<Token>& tokens, TokenKind kind) {
    for (const Token& token : tokens) {
        if (token.kind == kind) return token;
    }
    throw std::runtime_error("expected token missing");
}

int variable_named(const FunctionCfg& function, const std::string& name) {
    for (std::size_t i = 0; i < function.variables.size(); ++i) {
        if (function.variables[i].name == name) return static_cast<int>(i);
    }
    throw std::runtime_error("expected variable missing: " + name);
}

const Node& first_node(const FunctionCfg& function, NodeKind kind) {
    for (const Node& node : function.nodes) {
        if (node.kind == kind) return node;
    }
    throw std::runtime_error("expected CFG node missing");
}

std::int64_t coefficient(const LinearExpr& expression, int variable) {
    const auto it = expression.coefficients.find(variable);
    return it == expression.coefficients.end() ? 0 : it->second;
}

void add_run(std::vector<Test>& tests, const std::string& name,
             const std::string& source, ExecutionStatus expected,
             const std::string& input = "", int abort_id = 0) {
    tests.push_back({name, [source, expected, input, abort_id]() {
        expect_status(run(source, input), expected, abort_id);
    }});
}

void add_error(std::vector<Test>& tests, const std::string& name,
               const std::string& source, const std::string& category) {
    tests.push_back({name, [source, category]() { expect_error(source, category); }});
}

std::vector<Test> make_tests() {
    std::vector<Test> tests;
    const ExecutionStatus normal = ExecutionStatus::Normal;
    const ExecutionStatus abort = ExecutionStatus::Abort;
    const ExecutionStatus runtime_error = ExecutionStatus::RuntimeError;

    struct BodyCase { const char* name; const char* text; ExecutionStatus status; int abort_id; };
    const BodyCase body_cases[] = {
        {"runtime/empty-main", "", normal, 0},
        {"runtime/return-skips-abort", "return 7; abort;", normal, 0},
        {"runtime/abort-skips-halt", "abort; halt;", abort, 1},
        {"integer/leading-zero-is-decimal", "let x=010; if(x!=10){abort;}", normal, 0},
        {"integer/minimum-literal", "let x=-2147483648; if(x>=0){abort;}", normal, 0},
        {"integer/parenthesized-minimum", "let x=-((2147483648)); if(x!=-2147483648){abort;}", normal, 0},
        {"integer/zero-prefixed-minimum", "let x=-0002147483648; if(x!=-2147483648){abort;}", normal, 0},
        {"integer/double-negation-overflows", "let x=--2147483648;", runtime_error, 0},
        {"integer/intermediate-addition-overflows", "let x=2147483647+1-1;", runtime_error, 0},
        {"integer/minimum-subtraction-overflows", "let x=-2147483648-1;", runtime_error, 0},
        {"integer/dead-overflow-does-not-run", "halt; let x=2147483647+1;", normal, 0},
        {"integer/false-branch-overflow-does-not-run", "if(0==1){let x=2147483647+1;}", normal, 0},
        {"integer/comparison-does-not-subtract", "let x=-2147483648; let y=2147483647; if(x<y){halt;} abort;", normal, 0},
        {"integer/left-associative-subtraction", "let x=8-3-2; if(x!=3){abort;}", normal, 0},
        {"integer/subtract-negative-not-decrement", "let x=1; let y=x--1; if(y!=2){abort;} if(x!=1){abort;}", normal, 0},
        {"integer/add-positive-not-increment", "let x=1; let y=x++1; if(y!=2){abort;} if(x!=1){abort;}", normal, 0},
        {"integer/unary-precedence", "let x=-(2+3)- -+4; if(x!=-1){abort;}", normal, 0},
        {"condition/redundant-parentheses", "let x=1; if(((x==1))){halt;} abort;", normal, 0},
        {"assignment/old-value-used", "let x=1; x=x+1; if(x!=2){abort;}", normal, 0},
        {"control/else-if", "if(1==2){}else if(2==2){halt;} abort;", normal, 0},
        {"control/missing-else", "if(1==2){abort;} halt;", normal, 0},
        {"control/zero-iteration-loop", "while(0>1){abort;} halt;", normal, 0},
        {"scope/loop-local-reinitialized", "let x=0; while(x<3){let step=1; step=step+x; x=x+step;} if(x!=3){abort;}", normal, 0},
        {"scope/outer-assignment", "let x=1; {let y=x+1; x=y;} if(x!=2){abort;}", normal, 0},
        {"control/empty-statements", ";;; {} if(1==1){;} while(0==1){}", normal, 0},
        {"comparison/less", "if(1<2){halt;} abort;", normal, 0},
        {"comparison/less-equal", "if(2<=2){halt;} abort;", normal, 0},
        {"comparison/greater", "if(3>2){halt;} abort;", normal, 0},
        {"comparison/greater-equal", "if(2>=2){halt;} abort;", normal, 0},
        {"comparison/equal", "if(-2==-2){halt;} abort;", normal, 0},
        {"comparison/not-equal", "if(-2!=2){halt;} abort;", normal, 0}
    };
    for (const BodyCase& test : body_cases) {
        add_run(tests, test.name, body(test.text), test.status, "", test.abort_id);
    }

    struct ErrorCase { const char* name; const char* source; const char* category; };
    const ErrorCase error_cases[] = {
        {"literal/positive-minimum-magnitude", "let x=2147483648;", "literal_range_error"},
        {"literal/binary-minus-not-special", "let x=0-2147483648;", "literal_range_error"},
        {"literal/unary-plus-not-special", "let x=-(+2147483648);", "literal_range_error"},
        {"literal/no-range-folding", "let x=2147483648-1;", "literal_range_error"},
        {"literal/too-large-negative", "let x=-2147483649;", "literal_range_error"},
        {"literal/dead-invalid-literal", "halt; let x=2147483648;", "literal_range_error"},
        {"category/integer-condition", "let x=1; if(x){halt;}", "category_error"},
        {"category/comparison-assigned", "let x=1<2;", "category_error"},
        {"category/condition-arithmetic", "let x=(1<2)+1;", "category_error"},
        {"category/condition-returned", "return 1==1;", "category_error"},
        {"category/condition-unary", "if(-(1==1)){halt;}", "category_error"},
        {"syntax/comparison-chain", "if(1<2<3){halt;}", "syntax_error"},
        {"syntax/post-increment", "let x=1; x++;", "syntax_error"},
        {"syntax/empty-return", "return;", "syntax_error"},
        {"syntax/assignment-expression", "let x=(1=2);", "syntax_error"},
        {"syntax/required-if-braces", "if(1==1) halt;", "syntax_error"},
        {"syntax/required-while-braces", "while(1==1) ;", "syntax_error"},
        {"syntax/comment-not-identifier-paste", "let a/*comment*/b=1;", "syntax_error"},
        {"syntax/uninitialized-let", "let x;", "syntax_error"},
        {"syntax/reserved-local-name", "let halt=1;", "syntax_error"},
        {"name/self-initialization", "let x=x;", "name_error"},
        {"name/expired-block-variable", "{let y=1;} y=2;", "name_error"},
        {"name/undeclared-assignment", "x=1;", "name_error"},
        {"name/undeclared-input", "input(x);", "name_error"},
        {"name/disjoint-branch-redeclaration", "if(0==0){let x=1;}else{let x=2;}", "name_error"},
        {"name/dead-undefined-call", "halt; missing();", "name_error"},
        {"name/variable-not-function", "let f=1; f();", "name_error"}
    };
    for (const ErrorCase& test : error_cases) {
        add_error(tests, test.name, body(test.source), test.category);
    }
    add_error(tests, "literal/very-long-number", body("let x=" + std::string(200, '9') + ";"), "literal_range_error");
    add_error(tests, "entry/missing-main", "fn helper(){}", "entry_error");
    add_error(tests, "entry/main-has-parameter", "fn main(x){}", "entry_error");
    add_error(tests, "name/duplicate-function", "fn main(){} fn main(){}", "name_error");
    add_error(tests, "name/duplicate-parameter", "fn f(x,x){} fn main(){}", "name_error");
    add_error(tests, "name/parameter-redeclared", "fn f(x){{let x=1;}} fn main(){}", "name_error");
    add_error(tests, "arity/too-few-arguments", "fn f(x){} fn main(){f();}", "arity_error");
    add_error(tests, "arity/too-many-arguments", "fn f(x){} fn main(){f(1,2);}", "arity_error");
    add_error(tests, "category/condition-argument", "fn f(x){} fn main(){f(1<2);}", "category_error");
    add_error(tests, "syntax/top-level-variable", "let x=1; fn main(){}", "syntax_error");
    add_error(tests, "syntax/reserved-function-name", "fn return(){} fn main(){}", "syntax_error");
    add_error(tests, "syntax/trailing-parameter-comma", "fn f(x,){} fn main(){}", "syntax_error");
    add_error(tests, "syntax/trailing-argument-comma", "fn f(x){} fn main(){f(1,);}", "syntax_error");

    const char* invalid_tokens[] = {"12abc", "1_000", "0x10", "1e3", "1.0", "!1", "1*2", "1/2", "1&&2", "1||2"};
    for (std::size_t i = 0; i < sizeof(invalid_tokens) / sizeof(invalid_tokens[0]); ++i) {
        add_error(tests, "lexer/invalid-token-" + std::to_string(i + 1),
                  body(std::string("let x=") + invalid_tokens[i] + ";"), "lex_error");
    }
    add_error(tests, "lexer/unclosed-comment", "fn main(){} /* never closed", "lex_error");
    add_run(tests, "lexer/comments-and-whitespace", "/* first */\tfn\vmain\f() { // line\n halt; } // eof", normal);
    tests.push_back({"lexer/bom-byte-columns", []() {
        const std::string source = "\xEF\xBB\xBF" "fn main(){/*\xE4\xB8\xAD*/halt;}";
        const std::vector<Token> tokens = tokenize(source);
        const Token& fn = token_of(tokens, TokenKind::Fn);
        const Token& halt = token_of(tokens, TokenKind::Halt);
        require(fn.span.begin.offset == 3 && fn.span.begin.column == 1, "BOM changed first token location");
        require(halt.span.begin.offset == source.find("halt"), "wrong raw byte offset");
        require(halt.span.begin.column == source.find("halt") - 3 + 1, "column is not UTF-8 byte based");
        expect_status(run(source), ExecutionStatus::Normal);
    }});
    tests.push_back({"lexer/mixed-newlines", []() {
        const std::string source = "fn main(){\r\n  halt;\r abort;\n}";
        const std::vector<Token> tokens = tokenize(source);
        const Token& halt = token_of(tokens, TokenKind::Halt);
        const Token& abort_token = token_of(tokens, TokenKind::Abort);
        require(halt.span.begin.line == 2 && halt.span.begin.column == 3, "CRLF counted incorrectly");
        require(abort_token.span.begin.line == 3 && abort_token.span.begin.column == 2, "bare CR counted incorrectly");
        require(abort_token.span.begin.offset == source.find("abort"), "newline normalization lost byte offset");
    }});
    tests.push_back({"lexer/unclosed-comment-position", []() {
        try { (void)tokenize("fn main(){}\n  /*oops"); }
        catch (const Diagnostic& error) {
            require(error.category == "lex_error", "wrong diagnostic category");
            require(error.span.begin.line == 2 && error.span.begin.column == 3, "comment error should point at opening delimiter");
            return;
        }
        throw std::runtime_error("unclosed comment accepted");
    }});
    const std::string invalid_utf8[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xE4\xB8", "\x80"};
    for (std::size_t i = 0; i < sizeof(invalid_utf8) / sizeof(invalid_utf8[0]); ++i) {
        add_error(tests, "lexer/invalid-utf8-" + std::to_string(i + 1),
                  "fn main(){} // " + invalid_utf8[i], "lex_error");
    }
    add_error(tests, "lexer/non-ascii-identifier", "fn main(){let \xE4\xB8\xAD=1;}", "lex_error");
    add_error(tests, "lexer/noninitial-bom", "fn main(){}\xEF\xBB\xBF", "lex_error");

    const std::string input_program = body("let x=9; input(x); halt;");
    const char* valid_inputs[] = {"0", "+0", "-0", "0007", "2147483647", "-2147483648", " \t\r\n+007\n", "7 unused-token"};
    for (std::size_t i = 0; i < sizeof(valid_inputs) / sizeof(valid_inputs[0]); ++i) {
        add_run(tests, "input/valid-" + std::to_string(i + 1), input_program, normal, valid_inputs[i]);
    }
    const char* invalid_inputs[] = {"2147483648", "-2147483649", "12abc", "1,2", "+", "1.0", "", " \t\r\n", "99999999999999999999999999"};
    for (std::size_t i = 0; i < sizeof(invalid_inputs) / sizeof(invalid_inputs[0]); ++i) {
        add_run(tests, "input/invalid-" + std::to_string(i + 1), input_program, runtime_error, invalid_inputs[i]);
    }
    add_run(tests, "input/two-tokens-same-line", body("let a=0; let b=0; input(a); input(b); if(a!=3){abort;} if(b!=2){abort;}"), normal, "3 2");
    tests.push_back({"input/eof-error-code", [input_program]() {
        const ExecutionResult result = run(input_program);
        expect_status(result, ExecutionStatus::RuntimeError);
        require(result.code == "input_eof", "EOF not distinguished from invalid token");
    }});
    tests.push_back({"input/stream-failure", [input_program]() {
        ProgramCfg cfg = compile(input_program);
        std::istringstream input("1");
        input.setstate(std::ios::badbit);
        const ExecutionResult result = execute(cfg, input);
        expect_status(result, ExecutionStatus::RuntimeError);
        require(result.code == "input_failure", "I/O failure reported as EOF");
    }});
    tests.push_back({"input/malformed-overflow-error-code", [input_program]() {
        const ExecutionResult result = run(input_program, "999999999999999999999999abc");
        expect_status(result, ExecutionStatus::RuntimeError);
        require(result.code == "invalid_input", "invalid characters must take precedence over range");
    }});
    tests.push_back({"input/overflow-error-code", [input_program]() {
        const ExecutionResult result = run(input_program, "999999999999999999999999");
        expect_status(result, ExecutionStatus::RuntimeError);
        require(result.code == "input_out_of_range", "numeric overflow classified incorrectly");
    }});
    add_run(tests, "input/all-ascii-whitespace", input_program, normal, " \t\r\n\f\v+007\v");

    add_run(tests, "call/forward-call", "fn main(){if(next(3)!=4){abort;}} fn next(x){return x+1;}", normal);
    add_run(tests, "call/function-variable-namespaces", "fn id(id){return id;} fn main(){let id=4; if(id(id)!=4){abort;}}", normal);
    add_run(tests, "call/parameters-are-copies", "fn change(x){x=9; return x;} fn main(){let x=1; let y=change(x); if(x!=1){abort;} if(y!=9){abort;}}", normal);
    add_run(tests, "call/discard-return", "fn f(){return 9;} fn main(){f(); halt;}", normal);
    add_run(tests, "call/implicit-return-zero", "fn f(){;} fn main(){if(f()!=0){abort;}}", normal);
    add_run(tests, "call/multiple-returns", "fn f(x){if(x<0){return 1;}else if(x==0){return 2;}return 3;} fn main(){if(f(-1)+f(0)+f(1)!=6){abort;}}", normal);
    add_run(tests, "call/recursive-sum", "fn sum(n){if(n==0){return 0;}return n+sum(n-1);} fn main(){if(sum(10)!=55){abort;}}", normal);
    add_run(tests, "call/deep-explicit-stack", "fn count(n){if(n==0){return 0;}return count(n-1)+1;} fn main(){if(count(2000)!=2000){abort;}}", normal);
    add_run(tests, "call/mutual-recursion", "fn even(n){if(n==0){return 1;}return odd(n-1);} fn odd(n){if(n==0){return 0;}return even(n-1);} fn main(){if(even(8)!=1){abort;}if(odd(7)!=1){abort;}}", normal);
    add_run(tests, "call/recursive-local-frames", "fn f(n){let saved=n;if(n==0){return 0;}let child=f(n-1);if(saved!=n){abort;}return saved+child;}fn main(){if(f(5)!=15){abort;}}", normal);
    const std::string order_program = "fn read(){let v=0;input(v);return v;}fn sub(a,b){return a-b;}fn main(){if(sub(read(),read())!=1){abort;}}";
    add_run(tests, "call/argument-order-normal", order_program, normal, "3 2");
    add_run(tests, "call/argument-order-abort", order_program, abort, "2 3", 1);
    add_run(tests, "call/binary-order", "fn read(){let v=0;input(v);return v;}fn main(){if(read()-read()!=1){abort;}}", normal, "3 2");
    add_run(tests, "call/comparison-order", "fn read(){let v=0;input(v);return v;}fn main(){if(read()<read()){halt;}abort;}", normal, "1 2");
    add_run(tests, "call/assignment-update-after-rhs", "fn id(x){return x;}fn main(){let x=2;x=id(x)+id(x);if(x!=4){abort;}}", normal);
    add_run(tests, "call/condition-call-once", "fn read(){let v=0;input(v);return v;}fn main(){if(read()+read()!=3){abort;}}", normal, "1 2 invalid");
    add_run(tests, "loop/recompute-condition-call", "fn read(){let v=0;input(v);return v;}fn main(){while(read()>0){}let last=9;input(last);if(last!=7){abort;}}", normal, "2 1 0 7");
    add_run(tests, "call/nested-main-return", "fn main(){let x=0;input(x);if(x==0){return 0;}let result=main();abort;}", abort, "1 0", 1);
    add_run(tests, "termination/halt-propagates", "fn stop(){halt;}fn main(){let x=stop()+2147483647+1;abort;}", normal);
    add_run(tests, "termination/abort-propagates", "fn fail(){abort;}fn main(){let x=fail()+2147483647+1;abort;}", abort, "", 1);
    add_run(tests, "termination/argument-stops-outer-call", "fn stop(){halt;}fn fail(){abort;}fn combine(a,b){abort;}fn main(){combine(stop(),fail());abort;}", normal);
    add_run(tests, "termination/return-expression-halts", "fn stop(){halt;}fn wrap(){return stop();}fn main(){let x=wrap();abort;}", normal);
    tests.push_back({"runtime/overflow-location", []() {
        const std::string source = "fn main(){\n let x=2147483647 + 1;\n}";
        const ExecutionResult result = run(source);
        expect_status(result, ExecutionStatus::RuntimeError);
        require(result.code == "integer_overflow", "wrong overflow error code");
        require(result.span.begin.offset == source.find('+'), "overflow does not point at operator");
        require(result.span.begin.line == 2, "overflow line incorrect");
    }});
    tests.push_back({"resource/step-limit", []() {
        ExecutionOptions options;
        options.max_steps = 20;
        const ExecutionResult result = run(body("while(0==0){} abort;"), "", options);
        expect_status(result, ExecutionStatus::ResourceLimit);
        require(result.code == "step_limit" && result.steps == 20, "wrong step-limit accounting");
    }});
    tests.push_back({"resource/call-depth-limit", []() {
        ExecutionOptions options;
        options.max_call_depth = 8;
        const ExecutionResult result = run("fn f(){return f();}fn main(){f();}", "", options);
        expect_status(result, ExecutionStatus::ResourceLimit);
        require(result.code == "call_depth_limit", "recursion not stopped by call-depth limit");
    }});
    tests.push_back({"resource/zero-disables-limits", []() {
        ExecutionOptions options;
        options.max_call_depth = 0;
        options.max_steps = 0;
        expect_status(run(body("let x=0;while(x<10){x=x+1;}"), "", options), ExecutionStatus::Normal);
    }});

    tests.push_back({"cfg/abort-inventory-includes-dead-code", []() {
        const std::string source = "fn unused(){abort;}fn main(){abort;abort;}";
        const ProgramCfg cfg = compile(source);
        require(cfg.abort_sites.size() == 3, "dead abort omitted from inventory");
        for (std::size_t i = 0; i < cfg.abort_sites.size(); ++i) {
            require(cfg.abort_sites[i].id == static_cast<int>(i + 1), "abort numbering not one based");
            if (i) require(cfg.abort_sites[i-1].span.begin.offset < cfg.abort_sites[i].span.begin.offset, "abort IDs not in source order");
        }
        expect_status(run(source), ExecutionStatus::Abort, 2);
        std::size_t count = 0;
        for (const FunctionCfg& function : cfg.functions) {
            for (const Node& node : function.nodes) if (node.kind == NodeKind::Abort) ++count;
        }
        require(count == 3, "unreachable abort missing from CFG");
    }});
    tests.push_back({"cfg/nested-call-source-and-execution-order", []() {
        const std::string source = "fn outer(x){return x;}fn inner(){return 1;}fn main(){outer(inner());}";
        const ProgramCfg cfg = compile(source);
        require(cfg.call_sites.size() == 2, "wrong number of explicit call sites");
        require(cfg.call_sites[0].span.begin.offset == source.rfind("outer("), "outer must be C1");
        require(cfg.call_sites[1].span.begin.offset == source.rfind("inner("), "inner must be C2");
        const FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        std::vector<int> calls;
        int node_id = function.entry;
        std::size_t budget = function.nodes.size() + 1;
        while (node_id >= 0 && budget--) {
            const Node& node = function.nodes[static_cast<std::size_t>(node_id)];
            if (node.kind == NodeKind::Call) calls.push_back(node.call_id);
            node_id = node.next;
        }
        require(calls == std::vector<int>({2, 1}), "nested call execution order follows source IDs incorrectly");
    }});
    tests.push_back({"cfg/terminal-nodes-have-no-successor", []() {
        const ProgramCfg cfg = compile("fn f(){return 1;abort;}fn main(){halt;abort;}");
        for (const FunctionCfg& function : cfg.functions) {
            for (const Node& node : function.nodes) {
                if (node.kind == NodeKind::Return || node.kind == NodeKind::Halt || node.kind == NodeKind::Abort) {
                    require(node.next == -1 && node.true_target == -1 && node.false_target == -1, "terminal node has ordinary successor");
                }
            }
        }
    }});
    tests.push_back({"cfg/sum-condition-preserved", []() {
        const ProgramCfg cfg = compile(body("let x=1;let y=2;if(x+y<=10){halt;}abort;"));
        const FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        const Node& branch = first_node(function, NodeKind::Branch);
        require(branch.condition.valid, "linear condition absent");
        require(branch.comparison == CompareOp::LessEqual, "comparison changed");
        require(coefficient(branch.condition, variable_named(function,"x")) == 1, "x sum coefficient lost");
        require(coefficient(branch.condition, variable_named(function,"y")) == 1, "y sum coefficient lost");
        require(branch.condition.constant == -10, "sum comparison constant incorrect");
    }});
    tests.push_back({"cfg/difference-condition-preserved", []() {
        const ProgramCfg cfg = compile(body("let x=1;let y=2;if(x-y!=3){halt;}"));
        const FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        const Node& branch = first_node(function, NodeKind::Branch);
        require(branch.condition.valid && branch.comparison == CompareOp::NotEqual, "difference comparison missing");
        require(coefficient(branch.condition, variable_named(function,"x")) == 1, "x difference coefficient wrong");
        require(coefficient(branch.condition, variable_named(function,"y")) == -1, "y difference coefficient wrong");
        require(branch.condition.constant == -3, "difference constant wrong");
    }});
    tests.push_back({"cfg/call-results-are-distinct-linear-operands", []() {
        const ProgramCfg cfg = compile("fn f(){return 1;}fn main(){if(f()+f()==2){halt;}abort;}");
        const FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        const Node& branch = first_node(function, NodeKind::Branch);
        std::vector<int> targets;
        for (const Node& node : function.nodes) if (node.kind == NodeKind::Call) targets.push_back(node.target);
        require(targets.size() == 2 && targets[0] != targets[1], "two calls share a result temporary");
        require(branch.condition.valid, "call-result linear information missing");
        for (int target : targets) require(coefficient(branch.condition,target) == 1, "call result omitted from condition");
        require(branch.condition.constant == -2, "call condition constant wrong");
    }});
    tests.push_back({"cfg/block-local-cleanup", []() {
        const ProgramCfg cfg = compile(body("let x=1;{let y=x+1;x=y;}if(x!=2){abort;}"));
        const FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        const int y = variable_named(function,"y");
        bool found = false;
        for (const Node& node : function.nodes) {
            if (node.kind == NodeKind::Nop && std::find(node.forget.begin(), node.forget.end(), y) != node.forget.end()) found = true;
        }
        require(found, "scope exit does not forget block local");
    }});
    tests.push_back({"cfg/invalid-edge-rejected", []() {
        ProgramCfg cfg = compile(body("halt;"));
        FunctionCfg& function = cfg.functions[static_cast<std::size_t>(cfg.entry_function)];
        function.nodes[static_cast<std::size_t>(function.entry)].next = static_cast<int>(function.nodes.size() + 10);
        try { validate_cfg(cfg); }
        catch (const std::logic_error&) { return; }
        throw std::runtime_error("CFG validator accepted dangling edge");
    }});
    tests.push_back({"cfg/reproducible-dump", []() {
        const std::string source = "fn id(x){return x;}fn main(){let x=id(1);if(x==1){halt;}abort;}";
        std::ostringstream first, second;
        dump_cfg(compile(source), first);
        dump_cfg(compile(source), second);
        require(!first.str().empty() && first.str() == second.str(), "CFG text is empty or nondeterministic");
    }});

    tests.push_back({"frontend/nesting-limit", []() {
        expect_error(body("let x=" + std::string(1024, '(') + "1" + std::string(1024, ')') + ";"), "frontend_limit");
    }});
    tests.push_back({"frontend/left-associated-depth-limit", []() {
        std::string expression = "1";
        for (int i = 0; i < 1024; ++i) expression += "+1";
        expect_error(body("let x=" + expression + ";"), "frontend_limit");
    }});
    return tests;
}
} // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "Usage: fdulang_language_tests [name-substring|--list]\n";
        return 2;
    }
    const std::vector<Test> tests = make_tests();
    const std::string filter = argc == 2 ? argv[1] : "";
    if (filter == "--list") {
        for (const Test& test : tests) std::cout << test.name << '\n';
        return 0;
    }
    std::size_t selected = 0;
    std::size_t failures = 0;
    for (const Test& test : tests) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++selected;
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const Diagnostic& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": unexpected " << error.category
                      << " at " << error.span.begin.line << ':' << error.span.begin.column
                      << ": " << error.what() << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }
    if (!selected) {
        std::cerr << "No language tests match: " << filter << '\n';
        return 2;
    }
    std::cout << "Language tests: " << selected << ", passed: " << selected - failures
              << ", failed: " << failures << '\n';
    return failures ? 1 : 0;
}
