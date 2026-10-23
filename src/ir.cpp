#include "fdulang/ir.hpp"
#include "fdulang/ast.hpp"

#include <cassert>
#include <limits>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace fdulang {
namespace {

bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& result) {
    const auto lo = (std::numeric_limits<std::int64_t>::min)();
    const auto hi = (std::numeric_limits<std::int64_t>::max)();
    if ((b > 0 && a > hi - b) || (b < 0 && a < lo - b)) return false;
    result = a + b;
    return true;
}

bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t& result) {
    const auto lo = (std::numeric_limits<std::int64_t>::min)();
    const auto hi = (std::numeric_limits<std::int64_t>::max)();
    if ((b > 0 && a < lo + b) || (b < 0 && a > hi + b)) return false;
    result = a - b;
    return true;
}

LinearExpr unavailable_linear() {
    LinearExpr result;
    result.valid = false;
    return result;
}

LinearExpr operand_linear(const Operand& operand) {
    LinearExpr result;
    if (operand.is_constant) result.constant = operand.constant;
    else result.coefficients[operand.variable] = 1;
    return result;
}

// Metadata uses mathematical arithmetic, with a conservative fallback when its
// representation is insufficient. It never replaces concrete arithmetic nodes.
LinearExpr combine_linear(const LinearExpr& left, const LinearExpr& right,
                          bool subtract) {
    if (!left.valid || !right.valid) return unavailable_linear();
    LinearExpr result = left;
    const bool constant_ok = subtract
        ? checked_sub(left.constant, right.constant, result.constant)
        : checked_add(left.constant, right.constant, result.constant);
    if (!constant_ok) return unavailable_linear();
    for (const auto& term : right.coefficients) {
        const auto found = result.coefficients.find(term.first);
        const std::int64_t current = found == result.coefficients.end() ? 0 : found->second;
        std::int64_t coefficient = 0;
        const bool ok = subtract
            ? checked_sub(current, term.second, coefficient)
            : checked_add(current, term.second, coefficient);
        if (!ok) return unavailable_linear();
        if (coefficient == 0) result.coefficients.erase(term.first);
        else result.coefficients[term.first] = coefficient;
    }
    return result;
}

struct Evaluation {
    int entry = -1;
    Operand value;
    LinearExpr linear;
    std::vector<int> temporaries;
};

class Lowerer {
public:
    explicit Lowerer(const AstFunction& source) : source_(source) {
        function_.name = source.name;
        function_.span = source.span;
        function_.variables = source.variables;
        for (const auto& parameter : source.parameters) {
            function_.parameters.push_back(parameter.variable);
        }
    }

    FunctionCfg lower() {
        Node fallthrough;
        fallthrough.kind = NodeKind::Return;
        fallthrough.span = {source_.span.end, source_.span.end};
        fallthrough.left = Operand::literal(0);
        fallthrough.synthetic = true;
        const int exit = add_node(fallthrough);
        const int body = statement(*source_.body, exit);
        Node entry;
        entry.kind = NodeKind::Entry;
        entry.span = source_.span;
        entry.next = body;
        entry.synthetic = true;
        function_.entry = add_node(entry);
        return std::move(function_);
    }

private:
    const AstFunction& source_;
    FunctionCfg function_;
    int temporary_count_ = 0;

    int add_node(const Node& node) {
        const int id = static_cast<int>(function_.nodes.size());
        function_.nodes.push_back(node);
        function_.nodes.back().id = id;
        return id;
    }

    int temporary(const SourceSpan& span) {
        Variable variable;
        variable.name = "%t" + std::to_string(++temporary_count_);
        variable.span = span;
        variable.temporary = true;
        const int id = static_cast<int>(function_.variables.size());
        function_.variables.push_back(variable);
        return id;
    }

    int cleanup(const SourceSpan& span, int next, const std::vector<int>& variables = {}) {
        Node node;
        node.kind = NodeKind::Nop;
        node.span = span;
        node.next = next;
        node.forget = variables;
        node.synthetic = true;
        return add_node(node);
    }

    Evaluation expression(const Expr& expr, int next) {
        Evaluation result;
        result.entry = next;
        if (expr.kind == ExprKind::Integer) {
            assert(expr.integer >= (std::numeric_limits<std::int32_t>::min)() &&
                   expr.integer <= (std::numeric_limits<std::int32_t>::max)());
            result.value = Operand::literal(static_cast<std::int32_t>(expr.integer));
            result.linear = operand_linear(result.value);
            return result;
        }
        if (expr.kind == ExprKind::Variable) {
            result.value = Operand::slot(expr.variable);
            result.linear = operand_linear(result.value);
            return result;
        }
        assert(expr.kind != ExprKind::Compare);

        const int destination = temporary(expr.span);
        Node operation;
        operation.target = destination;
        operation.next = next;
        operation.span = expr.operation_span;

        if (expr.kind == ExprKind::Unary) {
            assert(expr.operation == '+' || expr.operation == '-');
            operation.kind = expr.operation == '-' ? NodeKind::Neg : NodeKind::Copy;
            const int node = add_node(operation);
            Evaluation operand = expression(*expr.left, node);
            function_.nodes[node].left = operand.value;
            result.entry = operand.entry;
            result.linear = expr.operation == '-'
                ? combine_linear(LinearExpr{}, operand.linear, true) : operand.linear;
            result.temporaries = std::move(operand.temporaries);
        } else if (expr.kind == ExprKind::Binary) {
            assert(expr.operation == '+' || expr.operation == '-');
            operation.kind = expr.operation == '+' ? NodeKind::Add : NodeKind::Sub;
            const int node = add_node(operation);
            Evaluation right = expression(*expr.right, node);
            Evaluation left = expression(*expr.left, right.entry);
            function_.nodes[node].left = left.value;
            function_.nodes[node].right = right.value;
            result.entry = left.entry;
            result.linear = combine_linear(left.linear, right.linear, expr.operation == '-');
            result.temporaries = std::move(left.temporaries);
            result.temporaries.insert(result.temporaries.end(),
                                      right.temporaries.begin(), right.temporaries.end());
        } else if (expr.kind == ExprKind::Call) {
            operation.kind = NodeKind::Call;
            operation.callee = expr.function;
            operation.call_id = expr.call_id;
            operation.arguments.resize(expr.arguments.size());
            const int node = add_node(operation);
            int continuation = node;
            // Build backwards, but the resulting edges evaluate left to right.
            for (std::size_t i = expr.arguments.size(); i > 0; --i) {
                Evaluation argument = expression(*expr.arguments[i - 1], continuation);
                function_.nodes[node].arguments[i - 1] = argument.value;
                continuation = argument.entry;
                result.temporaries.insert(result.temporaries.end(),
                                          argument.temporaries.begin(), argument.temporaries.end());
            }
            result.entry = continuation;
            result.linear = operand_linear(Operand::slot(destination));
        } else {
            throw std::logic_error("unknown expression during CFG lowering");
        }
        result.value = Operand::slot(destination);
        result.temporaries.push_back(destination);
        return result;
    }

    int condition(const Expr& expr, int true_cleanup, int false_cleanup) {
        assert(expr.kind == ExprKind::Compare);
        Node branch;
        branch.kind = NodeKind::Branch;
        branch.span = expr.operation_span;
        branch.comparison = expr.comparison;
        branch.true_target = true_cleanup;
        branch.false_target = false_cleanup;
        const int id = add_node(branch);
        Evaluation right = expression(*expr.right, id);
        Evaluation left = expression(*expr.left, right.entry);
        function_.nodes[id].left = left.value;
        function_.nodes[id].right = right.value;
        function_.nodes[id].condition = combine_linear(left.linear, right.linear, true);
        left.temporaries.insert(left.temporaries.end(), right.temporaries.begin(), right.temporaries.end());
        function_.nodes[true_cleanup].forget = left.temporaries;
        function_.nodes[false_cleanup].forget = left.temporaries;
        return left.entry;
    }

    int statement(const Stmt& stmt, int next) {
        Node node;
        node.span = stmt.span;
        node.next = next;
        switch (stmt.kind) {
        case StmtKind::Block: {
            int continuation = cleanup(stmt.span, next, stmt.scope_variables);
            for (auto it = stmt.statements.rbegin(); it != stmt.statements.rend(); ++it) {
                continuation = statement(**it, continuation);
            }
            return continuation;
        }
        case StmtKind::Let:
        case StmtKind::Assign: {
            const int after = cleanup(stmt.span, next);
            node.kind = NodeKind::Copy;
            node.target = stmt.variable;
            node.next = after;
            const int copy = add_node(node);
            Evaluation value = expression(*stmt.expression, copy);
            function_.nodes[copy].left = value.value;
            function_.nodes[after].forget = value.temporaries;
            return value.entry;
        }
        case StmtKind::Call: {
            const int after = cleanup(stmt.span, next);
            Evaluation value = expression(*stmt.expression, after);
            function_.nodes[after].forget = value.temporaries;
            return value.entry;
        }
        case StmtKind::Input:
            node.kind = NodeKind::Input;
            node.target = stmt.variable;
            return add_node(node);
        case StmtKind::If: {
            const int true_entry = statement(*stmt.body, next);
            const int false_entry = stmt.alternative ? statement(*stmt.alternative, next) : next;
            const int true_cleanup = cleanup(stmt.expression->span, true_entry);
            const int false_cleanup = cleanup(stmt.expression->span, false_entry);
            return condition(*stmt.expression, true_cleanup, false_cleanup);
        }
        case StmtKind::While: {
            const int header = cleanup(stmt.expression->span, -1);
            const int true_cleanup = cleanup(stmt.expression->span, -1);
            const int false_cleanup = cleanup(stmt.expression->span, next);
            const int condition_entry = condition(*stmt.expression, true_cleanup, false_cleanup);
            function_.nodes[header].next = condition_entry;
            const int body_entry = statement(*stmt.body, header);
            function_.nodes[true_cleanup].next = body_entry;
            return header;
        }
        case StmtKind::Return: {
            node.kind = NodeKind::Return;
            node.next = -1;
            const int return_node = add_node(node);
            Evaluation value = expression(*stmt.expression, return_node);
            function_.nodes[return_node].left = value.value;
            return value.entry;
        }
        case StmtKind::Halt:
            node.kind = NodeKind::Halt;
            node.next = -1;
            return add_node(node);
        case StmtKind::Abort:
            node.kind = NodeKind::Abort;
            node.next = -1;
            node.abort_id = stmt.abort_id;
            return add_node(node);
        case StmtKind::Empty:
            node.kind = NodeKind::Nop;
            return add_node(node);
        }
        throw std::logic_error("unknown statement during CFG lowering");
    }
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::logic_error("invalid CFG: " + message);
}

bool valid_index(int index, std::size_t size) {
    return index >= 0 && static_cast<std::size_t>(index) < size;
}

void check_span(const SourceSpan& span) {
    require(span.begin.offset <= span.end.offset, "reversed source span");
    require(span.begin.line > 0 && span.begin.column > 0 &&
            span.end.line > 0 && span.end.column > 0, "invalid source position");
}

void check_comparison(CompareOp comparison) {
    switch (comparison) {
    case CompareOp::Less: case CompareOp::LessEqual:
    case CompareOp::Greater: case CompareOp::GreaterEqual:
    case CompareOp::Equal: case CompareOp::NotEqual: return;
    }
    throw std::logic_error("invalid CFG: unknown comparison");
}

void check_sites(const std::vector<StaticSite>& sites, std::size_t functions) {
    for (std::size_t i = 0; i < sites.size(); ++i) {
        require(sites[i].id == static_cast<int>(i + 1), "non-contiguous static site IDs");
        require(valid_index(sites[i].function, functions), "invalid static site function");
        check_span(sites[i].span);
        if (i > 0) require(sites[i - 1].span.begin.offset < sites[i].span.begin.offset,
                           "static sites are not in source order");
    }
}

std::string variable_text(const FunctionCfg& function, int variable) {
    return "v" + std::to_string(variable) + "(" + function.variables[variable].name + ")";
}

std::string operand_text(const FunctionCfg& function, const Operand& operand) {
    return operand.is_constant ? std::to_string(operand.constant)
                               : variable_text(function, operand.variable);
}

void dump_linear(const FunctionCfg& function, const LinearExpr& expression, std::ostream& output) {
    if (!expression.valid) {
        output << "unavailable";
        return;
    }
    bool first = true;
    for (const auto& term : expression.coefficients) {
        if (!first) output << " + ";
        output << '(' << term.second << ")*" << variable_text(function, term.first);
        first = false;
    }
    if (!first) output << " + ";
    output << expression.constant;
}

} // namespace

ProgramCfg lower_to_cfg(const AstProgram& program) {
    ProgramCfg result;
    result.source_name = program.source_name;
    result.entry_function = program.entry_function;
    result.abort_sites = program.abort_sites;
    result.call_sites = program.call_sites;
    for (const auto& function : program.functions) {
        result.functions.push_back(Lowerer(function).lower());
    }
    validate_cfg(result);
    return result;
}

void validate_cfg(const ProgramCfg& program) {
    require(valid_index(program.entry_function, program.functions.size()), "invalid program entry");
    require(program.functions[program.entry_function].name == "main", "entry is not main");
    require(program.functions[program.entry_function].parameters.empty(), "main has parameters");
    check_sites(program.abort_sites, program.functions.size());
    check_sites(program.call_sites, program.functions.size());
    std::vector<bool> seen_aborts(program.abort_sites.size(), false);
    std::vector<bool> seen_calls(program.call_sites.size(), false);
    std::set<std::string> function_names;

    for (std::size_t fi = 0; fi < program.functions.size(); ++fi) {
        const auto& function = program.functions[fi];
        require(function_names.insert(function.name).second, "duplicate function name");
        check_span(function.span);
        require(valid_index(function.entry, function.nodes.size()), "invalid function entry");
        require(function.nodes[function.entry].kind == NodeKind::Entry, "entry node has wrong kind");
        std::set<int> parameters;
        std::set<std::string> variable_names;
        for (const auto& variable : function.variables) {
            require(!variable.name.empty() && variable_names.insert(variable.name).second,
                    "empty or duplicate variable name");
            check_span(variable.span);
        }
        for (int parameter : function.parameters) {
            require(valid_index(parameter, function.variables.size()), "invalid parameter slot");
            require(parameters.insert(parameter).second, "duplicate parameter slot");
            require(function.variables[parameter].parameter, "parameter slot is not marked as a parameter");
        }
        for (std::size_t vi = 0; vi < function.variables.size(); ++vi) {
            require(function.variables[vi].parameter == (parameters.count(static_cast<int>(vi)) != 0),
                    "parameter flag does not match parameter list");
        }

        auto check_variable = [&](int variable) {
            require(valid_index(variable, function.variables.size()), "invalid variable slot");
        };
        auto check_operand = [&](const Operand& operand) {
            if (!operand.is_constant) check_variable(operand.variable);
        };
        for (std::size_t ni = 0; ni < function.nodes.size(); ++ni) {
            const auto& node = function.nodes[ni];
            require(node.id == static_cast<int>(ni), "node ID does not match its index");
            check_span(node.span);
            if (node.kind == NodeKind::Branch) {
                require(node.next == -1, "branch has an ordinary next edge");
                require(valid_index(node.true_target, function.nodes.size()) &&
                        valid_index(node.false_target, function.nodes.size()), "invalid branch edge");
            } else {
                require(node.true_target == -1 && node.false_target == -1, "non-branch has branch edges");
                const bool terminal = node.kind == NodeKind::Return || node.kind == NodeKind::Halt ||
                                      node.kind == NodeKind::Abort;
                require(terminal ? node.next == -1 : valid_index(node.next, function.nodes.size()),
                        "invalid ordinary edge");
            }
            require(node.kind == NodeKind::Nop || node.forget.empty(), "cleanup attached to non-Nop");
            std::set<int> forgotten;
            for (int variable : node.forget) {
                check_variable(variable);
                require(forgotten.insert(variable).second, "duplicate cleanup slot");
            }
            switch (node.kind) {
            case NodeKind::Entry:
                require(node.id == function.entry, "extra Entry node");
                break;
            case NodeKind::Copy:
            case NodeKind::Neg:
                check_variable(node.target);
                check_operand(node.left);
                break;
            case NodeKind::Add:
            case NodeKind::Sub:
                check_variable(node.target);
                check_operand(node.left);
                check_operand(node.right);
                break;
            case NodeKind::Input:
                check_variable(node.target);
                break;
            case NodeKind::Branch:
                check_operand(node.left);
                check_operand(node.right);
                check_comparison(node.comparison);
                if (node.condition.valid) {
                    for (const auto& term : node.condition.coefficients) {
                        check_variable(term.first);
                        require(term.second != 0, "zero coefficient in normalized condition");
                    }
                }
                break;
            case NodeKind::Call: {
                check_variable(node.target);
                require(valid_index(node.callee, program.functions.size()), "invalid callee");
                require(node.arguments.size() == program.functions[node.callee].parameters.size(),
                        "wrong number of CFG call arguments");
                for (const auto& argument : node.arguments) check_operand(argument);
                require(node.call_id > 0 && static_cast<std::size_t>(node.call_id) <= seen_calls.size(),
                        "invalid call site ID");
                const auto& site = program.call_sites[static_cast<std::size_t>(node.call_id - 1)];
                require(!seen_calls[static_cast<std::size_t>(node.call_id - 1)], "duplicate call node");
                seen_calls[static_cast<std::size_t>(node.call_id - 1)] = true;
                require(site.function == static_cast<int>(fi) &&
                        site.span.begin.offset == node.span.begin.offset, "call site does not match node");
                break;
            }
            case NodeKind::Return:
                check_operand(node.left);
                break;
            case NodeKind::Abort: {
                require(node.abort_id > 0 && static_cast<std::size_t>(node.abort_id) <= seen_aborts.size(),
                        "invalid abort site ID");
                const auto& site = program.abort_sites[static_cast<std::size_t>(node.abort_id - 1)];
                require(!seen_aborts[static_cast<std::size_t>(node.abort_id - 1)], "duplicate abort node");
                seen_aborts[static_cast<std::size_t>(node.abort_id - 1)] = true;
                require(site.function == static_cast<int>(fi) &&
                        site.span.begin.offset == node.span.begin.offset, "abort site does not match node");
                break;
            }
            case NodeKind::Halt:
            case NodeKind::Nop:
                break;
            default:
                throw std::logic_error("invalid CFG: unknown node kind");
            }
        }
    }
    for (bool seen : seen_aborts) require(seen, "abort site has no CFG node");
    for (bool seen : seen_calls) require(seen, "call site has no CFG node");
}

void dump_cfg(const ProgramCfg& program, std::ostream& output) {
    output << "source " << program.source_name << '\n';
    output << "entry F" << program.entry_function << " (main)\n";
    output << "abort sites:\n";
    for (const auto& site : program.abort_sites) {
        output << "  A" << site.id << " F" << site.function << " @"
               << site.span.begin.line << ':' << site.span.begin.column << '\n';
    }
    output << "call sites:\n";
    for (const auto& site : program.call_sites) {
        output << "  C" << site.id << " F" << site.function << " @"
               << site.span.begin.line << ':' << site.span.begin.column << '\n';
    }
    for (std::size_t fi = 0; fi < program.functions.size(); ++fi) {
        const auto& function = program.functions[fi];
        output << "\nfunction F" << fi << ' ' << function.name << '(';
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            if (i != 0) output << ", ";
            output << variable_text(function, function.parameters[i]);
        }
        output << ") entry n" << function.entry << '\n';
        for (const auto& node : function.nodes) {
            output << "  n" << node.id << " @" << node.span.begin.line << ':'
                   << node.span.begin.column << ' ';
            switch (node.kind) {
            case NodeKind::Entry: output << "entry"; break;
            case NodeKind::Copy:
                output << variable_text(function, node.target) << " = copy " << operand_text(function, node.left);
                break;
            case NodeKind::Add:
            case NodeKind::Sub:
                output << variable_text(function, node.target) << " = "
                       << (node.kind == NodeKind::Add ? "add " : "sub ")
                       << operand_text(function, node.left) << ", " << operand_text(function, node.right);
                break;
            case NodeKind::Neg:
                output << variable_text(function, node.target) << " = neg " << operand_text(function, node.left);
                break;
            case NodeKind::Input: output << "input " << variable_text(function, node.target); break;
            case NodeKind::Branch:
                output << "branch " << operand_text(function, node.left) << ' '
                       << compare_text(node.comparison) << ' ' << operand_text(function, node.right)
                       << " ? n" << node.true_target << " : n" << node.false_target;
                output << " [linear: ";
                dump_linear(function, node.condition, output);
                if (node.condition.valid) output << ' ' << compare_text(node.comparison) << " 0";
                output << ']';
                break;
            case NodeKind::Call:
                output << variable_text(function, node.target) << " = call C" << node.call_id << " F"
                       << node.callee << ' ' << program.functions[node.callee].name << '(';
                for (std::size_t i = 0; i < node.arguments.size(); ++i) {
                    if (i != 0) output << ", ";
                    output << operand_text(function, node.arguments[i]);
                }
                output << ')';
                break;
            case NodeKind::Return: output << "return " << operand_text(function, node.left); break;
            case NodeKind::Halt: output << "halt"; break;
            case NodeKind::Abort: output << "abort A" << node.abort_id; break;
            case NodeKind::Nop:
                output << "nop";
                if (!node.forget.empty()) {
                    output << " forget{";
                    for (std::size_t i = 0; i < node.forget.size(); ++i) {
                        if (i != 0) output << ", ";
                        output << variable_text(function, node.forget[i]);
                    }
                    output << '}';
                }
                break;
            }
            if (node.synthetic) output << " [synthetic]";
            if (node.next >= 0) output << " -> n" << node.next;
            output << '\n';
        }
    }
}

} // namespace fdulang
