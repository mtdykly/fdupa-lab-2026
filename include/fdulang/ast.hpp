#ifndef FDULANG_AST_HPP
#define FDULANG_AST_HPP

#include "fdulang/variable.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fdulang {

enum class ExprKind { Integer, Variable, Unary, Binary, Compare, Call };
enum class ExprCategory { Integer, Condition };

struct Expr {
    ExprKind kind = ExprKind::Integer;
    ExprCategory category = ExprCategory::Integer;
    SourceSpan span;
    SourceSpan operation_span;
    std::int64_t integer = 0;
    std::string name;
    char operation = '+';
    CompareOp comparison = CompareOp::Equal;
    int variable = -1;
    int function = -1;
    int call_id = 0;
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
    std::vector<std::unique_ptr<Expr>> arguments;
};

enum class StmtKind { Let, Assign, Call, Input, If, While, Return,
                      Halt, Abort, Block, Empty };
struct Stmt {
    StmtKind kind = StmtKind::Empty;
    SourceSpan span;
    std::string name;
    int variable = -1;
    int abort_id = 0;
    std::unique_ptr<Expr> expression;
    std::unique_ptr<Stmt> body;
    std::unique_ptr<Stmt> alternative;
    std::vector<std::unique_ptr<Stmt>> statements;
    std::vector<int> scope_variables; // Declarations belonging directly to a block.
};

struct Parameter {
    std::string name;
    SourceSpan span;
    int variable = -1;
};
struct AstFunction {
    std::string name;
    SourceSpan span;
    std::vector<Parameter> parameters;
    std::vector<Variable> variables;
    std::unique_ptr<Stmt> body;
};
struct AstProgram {
    std::string source_name;
    std::vector<AstFunction> functions;
    int entry_function = -1;
    std::vector<StaticSite> abort_sites;
    std::vector<StaticSite> call_sites;
};

} // namespace fdulang
#endif
