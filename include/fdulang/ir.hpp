#ifndef FDULANG_IR_HPP
#define FDULANG_IR_HPP

#include "fdulang/variable.hpp"
#include <cstdint>
#include <iosfwd>
#include <map>
#include <vector>

namespace fdulang {

struct AstProgram;

struct Operand {
    bool is_constant = true;
    std::int32_t constant = 0;
    int variable = -1;
    static Operand literal(std::int32_t value) {
        Operand result; result.constant = value; return result;
    }
    static Operand slot(int variable) {
        Operand result; result.is_constant = false;
        result.variable = variable; return result;
    }
};

// Mathematical sum(coefficients[v] * v) + constant. A Branch's predicate
// compares this expression with zero. Invalid metadata must be ignored.
struct LinearExpr {
    bool valid = true;
    std::map<int, std::int64_t> coefficients;
    std::int64_t constant = 0;
};

enum class NodeKind { Entry, Copy, Add, Sub, Neg, Input, Branch, Call,
                      Return, Halt, Abort, Nop };
struct Node {
    int id = -1;
    NodeKind kind = NodeKind::Nop;
    SourceSpan span;
    int target = -1;
    Operand left;
    Operand right;
    CompareOp comparison = CompareOp::Equal;
    int next = -1;
    int true_target = -1;
    int false_target = -1;
    int callee = -1;
    int call_id = 0;
    int abort_id = 0;
    std::vector<Operand> arguments;
    LinearExpr condition;
    std::vector<int> forget; // Scope/temporary cleanup; no source-language effect.
    bool synthetic = false;
};
struct FunctionCfg {
    std::string name;
    SourceSpan span;
    std::vector<Variable> variables;
    std::vector<int> parameters;
    std::vector<Node> nodes;
    int entry = -1;
};
struct ProgramCfg {
    std::string source_name;
    std::vector<FunctionCfg> functions;
    int entry_function = -1;
    std::vector<StaticSite> abort_sites;
    std::vector<StaticSite> call_sites;
};

// The lowering boundary validates the CFG once; its consumers use that result.
ProgramCfg lower_to_cfg(const AstProgram& program);
void dump_cfg(const ProgramCfg& program, std::ostream& output);
// Checks indices, edge shapes, operand slots, parameter and static-site mappings.
// Throws std::logic_error for a compiler defect, never a source Diagnostic.
void validate_cfg(const ProgramCfg& program);

} // namespace fdulang
#endif
