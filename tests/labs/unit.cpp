#include "fdulang/analysis.hpp"
#include "fdulang/frontend.hpp"
#include "fdulang/interval.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace fdulang;
using State = std::unique_ptr<AbstractState>;
using Range = std::pair<std::int32_t, std::int32_t>;
constexpr std::int32_t minimum = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t maximum = std::numeric_limits<std::int32_t>::max();

struct Test { std::string name; std::function<void()> run; };

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void constant(AbstractState& state, int variable, std::int32_t value) {
    state.assign(variable, NodeKind::Copy, Operand::literal(value), Operand::literal(0));
}

State point(const std::vector<std::int32_t>& values) {
    auto result = make_interval_top(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        constant(*result, static_cast<int>(i), values[i]);
    return result;
}

// Build boxes through hulls of endpoints; no implementation-private interval API.
State box(const std::vector<Range>& values) {
    auto result = make_interval_top(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        constant(*result, static_cast<int>(i), values[i].first);
        auto endpoint = result->clone();
        constant(*endpoint, static_cast<int>(i), values[i].second);
        result->join_with(*endpoint);
    }
    return result;
}

void equal(const AbstractState& actual, const AbstractState& expected) {
    require(actual.leq(expected) && expected.leq(actual), "abstract states differ");
}

void contains(const AbstractState& state, const std::vector<std::int32_t>& values) {
    const auto concrete = point(values);
    require(concrete->leq(state), "abstract state lost a concrete state");
}

Node comparison(Operand left, CompareOp op, Operand right) {
    Node node;
    node.kind = NodeKind::Branch;
    node.left = left; node.right = right; node.comparison = op;
    node.condition.valid = false; // Exercise the mandatory operand fallback.
    return node;
}

bool compare(std::int32_t left, CompareOp op, std::int32_t right) {
    switch (op) {
    case CompareOp::Less: return left < right;
    case CompareOp::LessEqual: return left <= right;
    case CompareOp::Greater: return left > right;
    case CompareOp::GreaterEqual: return left >= right;
    case CompareOp::Equal: return left == right;
    case CompareOp::NotEqual: return left != right;
    }
    throw std::logic_error("unknown comparison");
}

ProgramCfg compile(const std::string& text) {
    return lower_to_cfg(parse_and_check("analysis-unit.fdulang", text));
}

AnalysisResult analyze(const std::string& source, const AnalysisOptions& options = {}) {
    return analyze_intra(compile(source), interval_domain(), options);
}

void expect(const std::string& source, const std::set<int>& ids) {
    const auto result = analyze(source);
    require(result.status == AnalysisStatus::Success, "solver did not converge");
    require(result.may_abort == ids, "unexpected may_abort set");
}

std::string main_body(const std::string& body) {
    return "fn main() { " + body + " }";
}

std::vector<Test> tests() {
    std::vector<Test> result;
    auto add = [&](const std::string& name, std::function<void()> test) {
        result.push_back({name, std::move(test)});
    };

    add("domain/lattice_extremes", [] {
        auto top = make_interval_top(2), bottom = make_interval_bottom(2);
        require(bottom->is_bottom() && !top->is_bottom(), "wrong lattice extremes");
        require(bottom->leq(*top) && !top->leq(*bottom), "wrong partial order");
        contains(*top, {minimum, maximum});
    });
    add("domain/zero_variable_states", [] {
        auto top = make_interval_top(0), bottom = make_interval_bottom(0);
        require(!top->is_bottom() && bottom->is_bottom(), "zero-dimensional state conflation");
        bottom->join_with(*top);
        equal(*bottom, *top);
        const auto projected = top->remap({});
        equal(*projected, *top);
    });
    add("domain/clone_independence", [] {
        auto original = point({1, 2});
        auto clone = original->clone();
        constant(*clone, 0, 8);
        equal(*original, *point({1, 2}));
        equal(*clone, *point({8, 2}));
    });
    add("domain/bottom_absorbs_transfer", [] {
        auto state = make_interval_bottom(2);
        constant(*state, 0, 2);
        state->assign(1, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        state->forget(0);
        state->assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(1)), true);
        require(state->is_bottom(), "transfer resurrected bottom");
        require(state->remap({Operand::slot(0), Operand::literal(7)})->is_bottom(),
                "remapping resurrected bottom");
    });
    add("domain/join_endpoint_hull", [] {
        auto state = point({1});
        state->join_with(*point({5}));
        for (int value = 1; value <= 5; ++value) contains(*state, {value});
        require(!point({0})->leq(*state) && !point({6})->leq(*state), "join enlarged stable bounds");
        auto before = state->clone();
        state->join_with(*make_interval_bottom(1));
        equal(*state, *before);
        state->join_with(*state);
        equal(*state, *before);
    });
    add("domain/join_bottom_identity", [] {
        auto state = make_interval_bottom(2);
        const auto value = point({-1, 7});
        state->join_with(*value);
        equal(*state, *value);
    });
    add("domain/join_commutative", [] {
        auto a = box({{-3, 1}, {4, 9}}), b = box({{-1, 8}, {2, 3}});
        auto left = a->clone(), right = b->clone();
        left->join_with(*b); right->join_with(*a);
        equal(*left, *right);
        require(a->leq(*left) && b->leq(*left), "join is not an upper bound");
    });
    add("domain/widen_upper_endpoint", [] {
        auto state = box({{0, 2}}), incoming = box({{0, 3}});
        state->widen_with(*incoming);
        equal(*state, *box({{0, maximum}}));
    });
    add("domain/widen_lower_endpoint", [] {
        auto state = box({{-2, 0}}), incoming = box({{-3, 0}});
        state->widen_with(*incoming);
        equal(*state, *box({{minimum, 0}}));
    });
    add("domain/widen_stability_and_bottom", [] {
        auto state = box({{-2, 5}}), before = state->clone();
        state->widen_with(*before); equal(*state, *before);
        state->widen_with(*make_interval_bottom(1)); equal(*state, *before);
        auto empty = make_interval_bottom(1);
        empty->widen_with(*before); equal(*empty, *before);
    });
    add("domain/forget_preserves_other_slots", [] {
        auto state = point({1, 2});
        state->forget(0);
        equal(*state, *box({{minimum, maximum}, {2, 2}}));
    });
    add("domain/copy_and_self_assignment", [] {
        auto state = point({2, 7});
        state->assign(0, NodeKind::Copy, Operand::slot(0), Operand::literal(0));
        equal(*state, *point({2, 7}));
        state->assign(0, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        equal(*state, *point({3, 7}));
        state->assign(1, NodeKind::Sub, Operand::slot(0), Operand::slot(1));
        equal(*state, *point({3, -4}));
        state->assign(0, NodeKind::Sub, Operand::slot(0), Operand::slot(0));
        equal(*state, *point({0, -4}));
        state->assign(1, NodeKind::Neg, Operand::slot(1), Operand::literal(0));
        equal(*state, *point({0, 4}));
    });
    add("domain/arithmetic_bounds", [] {
        auto state = box({{-2, 4}, {1, 3}, {0, 0}});
        state->assign(2, NodeKind::Add, Operand::slot(0), Operand::slot(1));
        equal(*state, *box({{-2, 4}, {1, 3}, {-1, 7}}));
        state->assign(2, NodeKind::Sub, Operand::slot(0), Operand::slot(1));
        equal(*state, *box({{-2, 4}, {1, 3}, {-5, 3}}));
        state->assign(2, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        equal(*state, *box({{-2, 4}, {1, 3}, {-4, 2}}));
    });
    add("domain/checked_arithmetic_endpoints", [] {
        auto state = box({{maximum - 1, maximum}, {0, 0}});
        state->assign(1, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        contains(*state, {maximum - 1, maximum});
        auto projected = state->remap({Operand::slot(1)});
        equal(*projected, *point({maximum}));
        state = point({maximum});
        state->assign(0, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        require(state->is_bottom(), "certain overflow still has normal results");
        state = point({minimum});
        state->assign(0, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        require(state->is_bottom(), "negating minimum has no normal result");
    });
    add("domain/partial_negation_overflow", [] {
        auto state = box({{minimum, minimum + 1}, {0, 0}});
        state->assign(1, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        contains(*state, {minimum + 1, maximum});
        equal(*state->remap({Operand::slot(1)}), *point({maximum}));
    });
    add("domain/remap_parallel_projection", [] {
        auto state = box({{1, 3}, {7, 9}});
        const auto mapped = state->remap({Operand::slot(1), Operand::slot(0), Operand::literal(-5), Operand::slot(1)});
        equal(*mapped, *box({{7, 9}, {1, 3}, {-5, -5}, {7, 9}}));
        equal(*state, *box({{1, 3}, {7, 9}}));
        require(!state->remap({})->is_bottom(), "projection lost a nonempty state");
    });
    add("domain/strict_endpoint_filters", [] {
        auto state = make_interval_top(1);
        state->assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(minimum)), true);
        require(state->is_bottom(), "x < INT_MIN is feasible");
        state = make_interval_top(1);
        state->assume(comparison(Operand::slot(0), CompareOp::Greater, Operand::literal(maximum)), true);
        require(state->is_bottom(), "x > INT_MAX is feasible");
        state = make_interval_top(1);
        state->assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(maximum)), false);
        equal(*state, *point({maximum}));
    });
    add("domain/equality_intersection", [] {
        auto state = box({{-3, 5}, {2, 9}});
        state->assume(comparison(Operand::slot(0), CompareOp::Equal, Operand::slot(1)), true);
        equal(*state, *box({{2, 5}, {2, 5}}));
        state = box({{-3, -1}, {2, 9}});
        state->assume(comparison(Operand::slot(0), CompareOp::Equal, Operand::slot(1)), true);
        require(state->is_bottom(), "disjoint intervals can be equal");
    });
    add("domain/disequality_is_a_union", [] {
        auto state = box({{-2, 2}});
        state->assume(comparison(Operand::slot(0), CompareOp::NotEqual, Operand::literal(0)), true);
        contains(*state, {-2}); contains(*state, {2});
        require(!state->is_bottom(), "disequality treated as conjunction");
        state = point({0});
        state->assume(comparison(Operand::slot(0), CompareOp::NotEqual, Operand::literal(0)), true);
        require(state->is_bottom(), "equal singleton passed disequality");
    });

    const std::vector<CompareOp> comparisons = {CompareOp::Less, CompareOp::LessEqual,
        CompareOp::Greater, CompareOp::GreaterEqual, CompareOp::Equal, CompareOp::NotEqual};
    for (std::size_t op_index = 0; op_index < comparisons.size(); ++op_index) {
        const auto op = comparisons[op_index];
        add("soundness/filter_" + std::to_string(op_index), [op] {
            for (bool truth : {false, true}) {
                auto source = box({{-3, 2}, {-1, 4}});
                auto filtered = source->clone();
                filtered->assume(comparison(Operand::slot(0), op, Operand::slot(1)), truth);
                require(filtered->leq(*source), "filter enlarged its input");
                for (int x = -3; x <= 2; ++x)
                    for (int y = -1; y <= 4; ++y)
                        if (compare(x, op, y) == truth) contains(*filtered, {x, y});
            }
        });
        add("soundness/same_slot_filter_" + std::to_string(op_index), [op] {
            for (bool truth : {false, true}) {
                auto state = box({{-3, 4}});
                state->assume(comparison(Operand::slot(0), op, Operand::slot(0)), truth);
                if (compare(0, op, 0) == truth) {
                    for (int value = -3; value <= 4; ++value) contains(*state, {value});
                } else require(state->is_bottom(), "impossible same-slot comparison survived");
            }
        });
    }
    for (NodeKind kind : {NodeKind::Add, NodeKind::Sub, NodeKind::Neg}) {
        add("soundness/transfer_" + std::to_string(static_cast<int>(kind)), [kind] {
            const std::vector<Range> cases = {{-3, 3}, {minimum, minimum + 2}, {maximum - 2, maximum}};
            for (const auto& left : cases) for (const auto& right : cases) {
                auto state = box({left, right, {0, 0}});
                state->assign(2, kind, Operand::slot(0), Operand::slot(1));
                for (std::int64_t x = left.first; x <= left.second; ++x)
                    for (std::int64_t y = right.first; y <= right.second; ++y) {
                        const std::int64_t value = kind == NodeKind::Add ? x + y
                            : kind == NodeKind::Sub ? x - y : -x;
                        if (value >= minimum && value <= maximum)
                            contains(*state, {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                              static_cast<std::int32_t>(value)});
                    }
            }
        });
    }
    add("soundness/linear_metadata", [] {
        const std::vector<std::int64_t> coefficients = {-2, -1, 0, 1, 2};
        for (const auto a : coefficients) for (const auto b : coefficients) {
            for (bool truth : {false, true}) {
                Node node = comparison(Operand::slot(0), CompareOp::LessEqual, Operand::literal(1));
                // Operands are valid already-evaluated results only if they match
                // metadata. Use a third slot to hold each concrete expression.
                auto state = box({{-2, 3}, {-3, 2}, {minimum, maximum}});
                node.left = Operand::slot(2);
                node.condition.valid = true;
                if (a != 0) node.condition.coefficients[0] = a;
                if (b != 0) node.condition.coefficients[1] = b;
                node.condition.constant = -1;
                state->assume(node, truth);
                for (int x = -2; x <= 3; ++x) for (int y = -3; y <= 2; ++y) {
                    const auto value = static_cast<std::int32_t>(a * x + b * y);
                    if ((value <= 1) == truth) contains(*state, {x, y, value});
                }
            }
        }
    });
    add("soundness/extreme_linear_bounds", [] {
        const auto lowest = std::numeric_limits<std::int64_t>::min();
        const auto highest = std::numeric_limits<std::int64_t>::max();
        for (const auto bound : {lowest, highest}) for (const auto coefficient : {-1, 1}) {
            for (const auto op : {CompareOp::Less, CompareOp::LessEqual, CompareOp::Greater,
                                  CompareOp::GreaterEqual, CompareOp::Equal, CompareOp::NotEqual}) {
                for (bool truth : {false, true}) {
                    // A trusted metadata bound far outside int32 fixes the sign
                    // regardless of x. Use a matching comparison operand sign.
                    const bool negative = bound == lowest;
                    auto node = comparison(Operand::literal(negative ? -1 : 1), op, Operand::literal(0));
                    node.condition.valid = true;
                    node.condition.constant = bound;
                    node.condition.coefficients[0] = coefficient;
                    auto state = make_interval_top(1);
                    state->assume(node, truth);
                    if (compare(negative ? -1 : 1, op, 0) == truth)
                        equal(*state, *make_interval_top(1));
                    else require(state->is_bottom(), "constant-sign affine condition survived");
                }
            }
        }
    });

    const std::vector<std::pair<std::string, std::set<int>>> programs = {
        {"abort;", {1}},
        {"halt; abort;", {}},
        {"return 0; abort;", {}},
        {"if (1 < 0) { abort; } halt;", {}},
        {"if (1 > 0) { abort; } halt;", {1}},
        {"let x=0; input(x); if (x < 0) { abort; } else { abort; }", {1, 2}},
        {"let x=0; input(x); if (x < -2147483648) { abort; } if (x > 2147483647) { abort; }", {}},
        {"let x=0; input(x); if (x == x) { halt; } abort;", {}},
        {"let x=3; x=x+1; if (x != 4) { abort; }", {}},
        {"let x=2147483647; x=x+1; abort;", {}},
        {"let x=0; input(x); if (x < 2147483646) { halt; } x=x+1; if (x==2147483647) { abort; }", {1}},
        {"while (1 == 1) { } abort;", {}},
        {"let x=0; while (x < 0) { abort; }", {}},
        {"let x=0; while (x < 5) { x=x+1; } if (x < 5) { abort; }", {}},
        {"let x=0; while (x < 3) { let y=0; y=y+1; if (y != 1) { abort; } x=x+1; }", {}},
        {"let x=0; while (x < 3) { let y=0; while (y < 2) { y=y+1; } x=x+1; } if (x<3) { abort; }", {}},
        {"let x=0; input(x); if (x==0) { x=1; } else { x=3; } if (x==2) { abort; }", {1}},
    };
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto program = programs[i];
        add("solver/intraprocedural_" + std::to_string(i), [program] {
            expect(main_body(program.first), program.second);
        });
    }
    add("solver/unreferenced_function", [] {
        expect("fn unused() { abort; } fn main() { halt; }", {});
    });
    add("solver/parameters_start_at_top", [] {
        expect("fn f(v) { if (v!=0) { abort; } return 0; } fn main() { let x=f(0); }", {1});
    });
    add("solver/call_result_top_preserves_caller_locals", [] {
        expect("fn f() { return 0; } fn main() { let x=3; let r=f(); if (x!=3) { abort; } if (r!=0) { abort; } }", {2});
    });
    add("solver/dead_call_is_in_static_call_graph", [] {
        expect("fn f() { abort; } fn main() { halt; let x=f(); abort; }", {1});
    });
    add("solver/call_may_return_after_callee_halt", [] {
        expect("fn f() { halt; } fn main() { let x=f(); abort; }", {1});
    });
    add("solver/transitive_static_calls", [] {
        const auto result = analyze("fn a() { abort; } fn b() { halt; let x=a(); } fn unused() { abort; } fn main() { let x=b(); }");
        require(result.status == AnalysisStatus::Success && result.may_abort == std::set<int>{1},
                "static transitive call graph differs");
        require(result.stats.functions == 3 && result.analyzed_functions.size() == 3,
                "wrong function selection");
    });
    add("solver/recursive_call_graph_terminates", [] {
        const auto result = analyze("fn f(n) { if(n==0) { abort; } return f(n-1); } fn main() { let x=f(3); }");
        require(result.status == AnalysisStatus::Success && result.may_abort == std::set<int>{1},
                "recursive intra analysis failed");
        require(result.stats.functions == 2, "recursive function selected repeatedly");
    });
    add("solver/mutual_recursive_call_graph_terminates", [] {
        expect("fn f(n) { if(n==0) { abort; } return g(n-1); } fn g(n) { return f(n); } fn main() { let x=f(3); }", {1});
    });
    add("solver/widening_and_determinism", [] {
        const auto source = main_body("let x=0; while (x>=0) { x=x+1; } abort;");
        AnalysisOptions options; options.widen_delay = 0;
        const auto first = analyze(source, options), second = analyze(source, options);
        require(first.status == AnalysisStatus::Success && first.may_abort.empty(),
                "growing loop did not converge soundly");
        require(first.stats.widenings > 0 && first.stats.widening_points > 0, "no loop widening");
        require(first.stats.iterations < 500, "widening failed to accelerate convergence");
        require(first.may_abort == second.may_abort && first.stats.iterations == second.stats.iterations
                && first.stats.widenings == second.stats.widenings, "non-deterministic solver");
    });
    add("solver/resource_limit_is_not_success", [] {
        AnalysisOptions options; options.max_iterations = 1;
        const auto result = analyze(main_body("abort;"), options);
        require(result.status == AnalysisStatus::ResourceLimit, "unfinished analysis reported success");
        require(!result.code.empty() && result.may_abort.empty(), "partial alerts presented as final output");
    });
    add("solver/zero_iteration_limit_disables_cap", [] {
        AnalysisOptions options; options.max_iterations = 0;
        const auto result = analyze(main_body("abort;"), options);
        require(result.status == AnalysisStatus::Success && result.may_abort == std::set<int>{1},
                "zero limit did not disable cap");
    });
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int executed = 0, passed = 0;
    for (const auto& test : tests()) {
        if (filter == "--list") { std::cout << test.name << '\n'; continue; }
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++executed;
        try {
            test.run(); ++passed;
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cout << "FAIL " << test.name << ": " << error.what() << '\n';
        }
    }
    if (filter == "--list") return 0;
    if (executed == 0) { std::cerr << "No matching analysis unit tests\n"; return 2; }
    std::cout << "Analysis unit tests: " << executed << ", passed: " << passed
              << ", failed: " << executed - passed << '\n';
    return passed == executed ? 0 : 1;
}
