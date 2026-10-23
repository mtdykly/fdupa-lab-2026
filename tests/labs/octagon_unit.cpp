#include "fdulang/analysis.hpp"
#include "fdulang/frontend.hpp"
#include "fdulang/octagon.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace fdulang;
using Range = std::pair<std::int32_t, std::int32_t>;
constexpr std::int32_t minimum = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t maximum = std::numeric_limits<std::int32_t>::max();
struct Test { std::string name; std::function<void()> run; };

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
const OctagonState& octagon(const AbstractState& value) {
    const auto* result = dynamic_cast<const OctagonState*>(&value);
    if (!result) throw std::runtime_error("not an octagon state");
    return *result;
}
void equal(const AbstractState& a, const AbstractState& b) {
    require(a.leq(b) && b.leq(a), "abstract states differ");
}
void bounds(OctagonState& state, int variable, std::int32_t lower, std::int32_t upper) {
    state.constrain(variable, 1, -1, 1, upper);
    state.constrain(variable, -1, -1, 1, -static_cast<std::int64_t>(lower));
}
OctagonState box(const std::vector<Range>& ranges) {
    OctagonState result(ranges.size());
    for (std::size_t i = 0; i < ranges.size(); ++i)
        bounds(result, static_cast<int>(i), ranges[i].first, ranges[i].second);
    return result;
}
OctagonState point(const std::vector<std::int32_t>& values) {
    OctagonState result(values.size());
    for (std::size_t i = 0; i < values.size(); ++i)
        bounds(result, static_cast<int>(i), values[i], values[i]);
    return result;
}
void contains(const AbstractState& state, const std::vector<std::int32_t>& values) {
    require(octagon(state).contains(values), "abstract operation lost a normal concrete state");
}
void range_is(const OctagonState& state, int variable, std::int64_t lower, std::int64_t upper) {
    const auto actual = state.value(variable);
    require(!state.is_bottom() && actual.lower == lower && actual.upper == upper,
            "unexpected projected interval");
}
std::size_t signed_index(int variable, int sign) {
    return static_cast<std::size_t>(2 * variable + (sign < 0 ? 1 : 0));
}
void bound_is(const OctagonState& state, std::size_t p, std::size_t q, std::int64_t value) {
    const auto actual = state.difference_bound(p, q);
    require(!actual.infinite && actual.value == value, "unexpected difference bound");
}
Node comparison(Operand left, CompareOp op, Operand right) {
    Node node;
    node.kind = NodeKind::Branch;
    node.left = left; node.right = right; node.comparison = op;
    node.condition.valid = false;
    return node;
}
bool compare(std::int64_t left, CompareOp op, std::int64_t right) {
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
AnalysisResult analyze(const std::string& source, const AnalysisOptions& options = {}) {
    auto cfg = lower_to_cfg(parse_and_check("octagon-unit.fdulang", source));
    return analyze_intra(cfg, octagon_domain(), options);
}
void expect(const std::string& source, const std::set<int>& ids) {
    const auto result = analyze(source);
    require(result.status == AnalysisStatus::Success, "octagon solver did not converge");
    require(result.may_abort == ids, "unexpected may_abort set");
}

std::vector<Test> tests() {
    std::vector<Test> result;
    auto add = [&](const std::string& name, std::function<void()> test) {
        result.push_back({name, std::move(test)});
    };
    add("support/tagged_infinity_and_floor_half", [] {
        const DbmBound infinity;
        const auto finite = DbmBound::finite(-1);
        require(bound_leq(finite, infinity) && !bound_leq(infinity, finite), "bad bound order");
        require(bound_min(finite, infinity).value == -1 && bound_max(finite, infinity).infinite,
                "bad tagged bound arithmetic");
        require(floor_half(-1) == -1 && floor_half(-3) == -2 && floor_half(3) == 1,
                "negative odd bound truncated toward zero");
        require(floor_half(std::numeric_limits<std::int64_t>::min()) ==
                    std::numeric_limits<std::int64_t>::min() / 2, "floor half overflow");
    });
    add("domain/extremes_and_zero_dimension", [] {
        OctagonState top(2), bottom(2, true);
        require(bottom.leq(top) && !top.leq(bottom), "wrong lattice order");
        contains(top, {minimum, maximum}); contains(top, {maximum, minimum});
        range_is(top, 0, minimum, maximum);
        OctagonState empty_top(0), empty_bottom(0, true);
        require(empty_top.contains({}) && !empty_bottom.contains({}), "zero-dimensional bottom conflated");
        equal(*top.remap({}), empty_top);
        equal(*bottom.remap({}), empty_bottom);
    });
    add("support/capacity_is_an_exception", [] {
        bool raised = false;
        try { OctagonState too_large(OctagonState::max_variables + 1); }
        catch (const OctagonResourceLimit&) { raised = true; }
        require(raised, "capacity exhaustion did not report a resource failure");
    });
    add("domain/bottom_absorbs_transfers", [] {
        OctagonState state(2, true);
        state.constrain(0, 1, 1, 1, 0);
        state.assign(0, NodeKind::Copy, Operand::literal(3), Operand::literal(0));
        state.forget(1);
        state.assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(9)), true);
        require(state.is_bottom(), "transfer resurrected bottom");
        require(state.remap({Operand::slot(1), Operand::literal(4)})->is_bottom(), "remap resurrected bottom");
    });
    add("domain/clone_independence", [] {
        auto state = point({1, 2});
        auto clone = state.clone();
        clone->assign(0, NodeKind::Copy, Operand::literal(9), Operand::literal(0));
        contains(state, {1, 2}); require(!state.contains({9, 2}), "clone changed original");
        contains(*clone, {9, 2});
    });
    for (const int sx : {-1, 1}) for (const int sy : {-1, 1}) {
        add("encoding/signs_" + std::to_string(sx) + "_" + std::to_string(sy), [sx, sy] {
            auto state = box({{-3, 3}, {-3, 3}});
            state.constrain(0, sx, 1, sy, 1);
            const auto p = signed_index(0, sx), q = signed_index(1, -sy);
            bound_is(state, p, q, 1);
            bound_is(state, q ^ 1, p ^ 1, 1);
            for (int x = -3; x <= 3; ++x) for (int y = -3; y <= 3; ++y)
                require(state.contains({x, y}) == (sx * x + sy * y <= 1), "wrong signed constraint encoding");
        });
    }
    add("closure/all_entries_coherent", [] {
        auto state = box({{-2, 5}, {-3, 7}, {-4, 9}});
        state.constrain(0, 1, 1, 1, 3);
        state.constrain(1, -1, 2, 1, 4);
        for (std::size_t i = 0; i < 6; ++i) for (std::size_t j = 0; j < 6; ++j) {
            const auto a = state.difference_bound(i, j), b = state.difference_bound(j ^ 1, i ^ 1);
            require(bound_leq(a, b) && bound_leq(b, a), "closure broke coherence");
            if (i == j) bound_is(state, i, j, 0);
        }
    });
    add("closure/negative_odd_unary", [] {
        OctagonState state(1);
        state.constrain(0, 1, 0, 1, -1);
        range_is(state, 0, minimum, -1);
        bound_is(state, 0, 1, -2);
        state.constrain(0, -1, -1, 1, 0);
        require(state.is_bottom(), "integer infeasible half interval survived");
    });
    add("closure/positive_odd_unary", [] {
        OctagonState state(1);
        state.constrain(0, -1, 0, -1, -3);
        range_is(state, 0, 2, maximum);
        bound_is(state, 1, 0, -4);
    });
    add("closure/integer_only_inconsistency", [] {
        OctagonState state(2);
        state.constrain(0, 1, 1, -1, 0);
        state.constrain(0, -1, 1, 1, 0);
        state.constrain(0, 1, 1, 1, 1);
        state.constrain(0, -1, 1, -1, -1);
        require(state.is_bottom(), "x=y and x+y=1 admitted integer values");
    });
    add("encoding/cancelled_same_variable", [] {
        OctagonState state(1);
        state.constrain(0, 1, 0, -1, 0);
        equal(state, OctagonState(1));
        state.constrain(0, -1, 0, 1, -1);
        require(state.is_bottom(), "0 <= -1 survived");
    });
    add("encoding/extreme_constraint_bounds", [] {
        OctagonState state(2);
        state.constrain(0, 1, 1, 1, std::numeric_limits<std::int64_t>::max());
        equal(state, OctagonState(2));
        state.constrain(0, -1, 1, -1, std::numeric_limits<std::int64_t>::min());
        require(state.is_bottom(), "impossible extreme bound survived");
    });
    add("closure/unary_strengthening", [] {
        const auto state = box({{-2, 3}, {-4, 5}});
        bound_is(state, 0, 3, 8); // x+y <= 8
        bound_is(state, 1, 2, 6); // -x-y <= 6
        bound_is(state, 0, 2, 7); // x-y <= 7
    });
    for (const int dimensions : {2, 3}) {
        add("closure/exhaustive_bounded_" + std::to_string(dimensions) + "d", [dimensions] {
            struct Constraint { int x, sx, y, sy, bound; };
            std::uint32_t seed = 0x6f637461u;
            auto next = [&seed]() {
                // Unsigned wraparound is defined; the seed is platform independent.
                seed = seed * 1664525u + 1013904223u;
                return seed;
            };
            std::vector<std::vector<std::int32_t>> universe;
            for (int x = -2; x <= 2; ++x) for (int y = -2; y <= 2; ++y) {
                if (dimensions == 2) universe.push_back({x, y});
                else for (int z = -2; z <= 2; ++z) universe.push_back({x, y, z});
            }
            // The initial box makes this a complete finite enumeration, not a
            // random search for counterexamples or a truth label for Lab scoring.
            for (int trial = 0; trial < 100; ++trial) {
                auto state = box(std::vector<Range>(static_cast<std::size_t>(dimensions), {-2, 2}));
                std::vector<Constraint> conjunction;
                const auto witness = universe[next() % universe.size()];
                for (int step = 0; step < 7; ++step) {
                    Constraint c;
                    c.x = static_cast<int>(next() % static_cast<unsigned>(dimensions));
                    c.sx = (next() & 1u) ? 1 : -1;
                    c.y = static_cast<int>(next() % static_cast<unsigned>(dimensions + 1)) - 1;
                    c.sy = (next() & 1u) ? 1 : -1;
                    c.bound = static_cast<int>(next() % 11u) - 5;
                    if (trial % 2 == 0) {
                        // Half the trials retain a witness throughout, exercising
                        // canonical bounds after long consistent conjunctions.
                        c.bound = c.sx * witness[static_cast<std::size_t>(c.x)] +
                            (c.y < 0 ? 0 : c.sy * witness[static_cast<std::size_t>(c.y)]) +
                            static_cast<int>(next() % 3u);
                    }
                    conjunction.push_back(c);
                    state.constrain(c.x, c.sx, c.y, c.sy, c.bound);
                    std::vector<std::vector<std::int32_t>> concrete;
                    for (const auto& value : universe) {
                        bool admitted = true;
                        for (const auto& guard : conjunction) {
                            const int sum = guard.sx * value[static_cast<std::size_t>(guard.x)] +
                                (guard.y < 0 ? 0 : guard.sy * value[static_cast<std::size_t>(guard.y)]);
                            if (sum > guard.bound) { admitted = false; break; }
                        }
                        require(state.contains(value) == admitted,
                                "closed DBM differs from complete concrete constraint enumeration");
                        if (admitted) concrete.push_back(value);
                    }
                    require(state.is_bottom() == concrete.empty(),
                            "integer satisfiability differs from complete bounded enumeration");
                    if (concrete.empty()) continue;
                    for (std::size_t p = 0; p < state.size() * 2; ++p)
                        for (std::size_t q = 0; q < state.size() * 2; ++q) {
                            std::int64_t greatest = std::numeric_limits<std::int64_t>::min();
                            for (const auto& value : concrete) {
                                const std::int64_t left = (p % 2 == 0 ? 1 : -1) * value[p / 2];
                                const std::int64_t right = (q % 2 == 0 ? 1 : -1) * value[q / 2];
                                if (left - right > greatest) greatest = left - right;
                            }
                            bound_is(state, p, q, greatest);
                        }
                }
            }
        });
    }
    add("forget/retains_eliminated_path_relations", [] {
        OctagonState state(3);
        state.constrain(0, 1, 1, 1, 3);
        state.constrain(1, -1, 2, -1, -5);
        state.forget(1);
        bound_is(state, 0, 4, -2);
        contains(state, {0, 1000, 2});
        require(!state.contains({1, 1000, 2}), "forget erased relation among surviving variables");
        range_is(state, 1, minimum, maximum);
    });
    add("join/preserves_sum_relation", [] {
        auto state = point({0, 10});
        const auto endpoint = point({10, 0});
        state.join_with(endpoint);
        for (int x = 0; x <= 10; ++x) contains(state, {x, 10 - x});
        require(!state.contains({5, 4}) && !state.contains({5, 6}), "join lost a stable sum relation");
        bound_is(state, 0, 3, 10); bound_is(state, 1, 2, -10);
        auto reversed = endpoint; reversed.join_with(point({0, 10}));
        equal(state, reversed);
    });
    add("join/bottom_and_idempotence", [] {
        auto state = point({2, -3});
        const auto before = state;
        state.join_with(OctagonState(2, true)); equal(state, before);
        state.join_with(state); equal(state, before);
        OctagonState empty(2, true); empty.join_with(state); equal(empty, state);
    });
    add("assignment/affine_self_shift_and_negation", [] {
        auto state = box({{-2, 3}, {0, 0}});
        state.assign(1, NodeKind::Add, Operand::slot(0), Operand::literal(5));
        bound_is(state, 2, 0, 5); bound_is(state, 0, 2, -5);
        state.assign(0, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        bound_is(state, 2, 0, 4); bound_is(state, 0, 2, -4);
        state.assign(0, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        bound_is(state, 0, 3, 4); bound_is(state, 1, 2, -4);
        for (int old = -2; old <= 3; ++old) contains(state, {-old - 1, old + 5});
    });
    add("assignment/constant_minus_self", [] {
        auto state = box({{0, 10}, {0, 0}});
        state.assign(1, NodeKind::Copy, Operand::slot(0), Operand::literal(0));
        state.assign(0, NodeKind::Sub, Operand::literal(10), Operand::slot(0));
        bound_is(state, 0, 3, 10); bound_is(state, 1, 2, -10);
        for (int old = 0; old <= 10; ++old) contains(state, {10 - old, old});
    });
    add("assignment/overwrite_forgets_old_target", [] {
        auto state = box({{-2, 3}, {0, 0}});
        state.assign(1, NodeKind::Add, Operand::slot(0), Operand::literal(2));
        state.assign(0, NodeKind::Copy, Operand::literal(0), Operand::literal(0));
        for (int y = 0; y <= 5; ++y) contains(state, {0, y});
        range_is(state, 0, 0, 0); range_is(state, 1, 0, 5);
    });
    add("assignment/checked_negation_and_shift", [] {
        auto state = box({{minimum, minimum + 1}, {0, 0}});
        state.assign(1, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        contains(state, {minimum + 1, maximum});
        range_is(state, 1, maximum, maximum);
        auto overflow = point({minimum});
        overflow.assign(0, NodeKind::Neg, Operand::slot(0), Operand::literal(0));
        require(overflow.is_bottom(), "certain negation overflow survived");
        overflow = point({maximum});
        overflow.assign(0, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        require(overflow.is_bottom(), "certain addition overflow survived");
        state = box({{maximum - 1, maximum}, {0, 0}});
        state.assign(1, NodeKind::Add, Operand::slot(0), Operand::literal(1));
        contains(state, {maximum - 1, maximum});
        range_is(state, 1, maximum, maximum);
    });
    add("assignment/same_slot_subtraction", [] {
        auto state = box({{minimum, maximum}, {-2, 2}});
        state.assign(0, NodeKind::Sub, Operand::slot(0), Operand::slot(0));
        range_is(state, 0, 0, 0); range_is(state, 1, -2, 2);
    });
    for (const auto kind : {NodeKind::Add, NodeKind::Sub, NodeKind::Neg}) {
        add("soundness/arithmetic_" + std::to_string(static_cast<int>(kind)), [kind] {
            const std::vector<Range> ranges = {{-3, 3}, {minimum, minimum + 2}, {maximum - 2, maximum}};
            for (const auto& a : ranges) for (const auto& b : ranges) {
                auto state = box({a, b, {0, 0}});
                state.assign(2, kind, Operand::slot(0), Operand::slot(1));
                for (std::int64_t x = a.first; x <= a.second; ++x)
                    for (std::int64_t y = b.first; y <= b.second; ++y) {
                        const auto value = kind == NodeKind::Add ? x + y : kind == NodeKind::Sub ? x - y : -x;
                        if (value >= minimum && value <= maximum)
                            contains(state, {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y),
                                              static_cast<std::int32_t>(value)});
                    }
            }
        });
    }
    add("soundness/general_alias_fallback", [] {
        for (const auto kind : {NodeKind::Add, NodeKind::Sub}) {
            auto state = box({{-3, 3}, {-2, 2}});
            state.assign(0, kind, Operand::slot(0), Operand::slot(1));
            for (int x = -3; x <= 3; ++x) for (int y = -2; y <= 2; ++y)
                contains(state, {kind == NodeKind::Add ? x + y : x - y, y});
        }
    });
    const std::vector<CompareOp> comparisons = {CompareOp::Less, CompareOp::LessEqual,
        CompareOp::Greater, CompareOp::GreaterEqual, CompareOp::Equal, CompareOp::NotEqual};
    for (std::size_t index = 0; index < comparisons.size(); ++index) {
        const auto op = comparisons[index];
        add("soundness/filter_" + std::to_string(index), [op] {
            for (const bool truth : {false, true}) {
                const auto before = box({{-3, 3}, {-2, 4}});
                auto state = before;
                state.assume(comparison(Operand::slot(0), op, Operand::slot(1)), truth);
                require(state.leq(before), "filter enlarged input");
                for (int x = -3; x <= 3; ++x) for (int y = -2; y <= 4; ++y)
                    if (compare(x, op, y) == truth) contains(state, {x, y});
            }
        });
        add("filter/same_slot_" + std::to_string(index), [op] {
            for (const bool truth : {false, true}) {
                auto state = box({{-3, 3}});
                state.assume(comparison(Operand::slot(0), op, Operand::slot(0)), truth);
                if (compare(0, op, 0) == truth) equal(state, box({{-3, 3}}));
                else require(state.is_bottom(), "impossible same-slot comparison survived");
            }
        });
    }
    add("filter/strict_integer_endpoints", [] {
        OctagonState state(1);
        state.assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(minimum)), true);
        require(state.is_bottom(), "x < INT_MIN survived");
        state = OctagonState(1);
        state.assume(comparison(Operand::slot(0), CompareOp::Greater, Operand::literal(maximum)), true);
        require(state.is_bottom(), "x > INT_MAX survived");
        state = OctagonState(1);
        state.assume(comparison(Operand::slot(0), CompareOp::Less, Operand::literal(maximum)), false);
        range_is(state, 0, maximum, maximum);
    });
    add("filter/disequality_keeps_both_sides", [] {
        auto state = box({{-2, 2}});
        state.assume(comparison(Operand::slot(0), CompareOp::NotEqual, Operand::literal(0)), true);
        contains(state, {-2}); contains(state, {2});
        state = point({0});
        state.assume(comparison(Operand::slot(0), CompareOp::NotEqual, Operand::literal(0)), true);
        require(state.is_bottom(), "equal singleton survived disequality");
    });
    add("soundness/linear_metadata_four_signs", [] {
        for (const int sx : {-1, 1}) for (const int sy : {-1, 1})
            for (const auto op : {CompareOp::Less, CompareOp::LessEqual, CompareOp::Equal, CompareOp::NotEqual})
                for (const bool truth : {false, true}) {
                    auto state = box({{-2, 3}, {-3, 2}, {minimum, maximum}});
                    auto node = comparison(Operand::slot(2), op, Operand::literal(1));
                    node.condition.valid = true;
                    node.condition.coefficients[0] = sx;
                    node.condition.coefficients[1] = sy;
                    node.condition.constant = -1;
                    state.assume(node, truth);
                    for (int x = -2; x <= 3; ++x) for (int y = -3; y <= 2; ++y) {
                        const int evaluated = sx * x + sy * y;
                        if (compare(evaluated, op, 1) == truth) contains(state, {x, y, evaluated});
                    }
                }
    });
    add("soundness/general_metadata_fallback", [] {
        for (const bool truth : {false, true}) {
            auto state = box({{-2, 2}, {-2, 2}, {-2, 2}, {minimum, maximum}});
            auto node = comparison(Operand::slot(3), CompareOp::LessEqual, Operand::literal(1));
            node.condition.valid = true;
            node.condition.coefficients = {{0, 2}, {1, -1}, {2, 1}};
            node.condition.constant = -1;
            state.assume(node, truth);
            for (int x = -2; x <= 2; ++x) for (int y = -2; y <= 2; ++y)
                for (int z = -2; z <= 2; ++z) {
                    const int evaluated = 2 * x - y + z;
                    if ((evaluated <= 1) == truth) contains(state, {x, y, z, evaluated});
                }
        }
    });
    add("remap/parallel_relations_constants_and_duplicates", [] {
        auto state = box({{0, 10}, {0, 0}});
        state.assign(1, NodeKind::Sub, Operand::literal(10), Operand::slot(0));
        const auto mapped = state.remap({Operand::slot(1), Operand::slot(0), Operand::slot(1), Operand::literal(-5)});
        for (int x = 0; x <= 10; ++x) contains(*mapped, {10 - x, x, 10 - x, -5});
        const auto& view = octagon(*mapped);
        bound_is(view, 0, 4, 0); bound_is(view, 4, 0, 0);
        bound_is(view, 0, 3, 10); bound_is(view, 1, 2, -10);
        require(!view.contains({5, 5, 4, -5}), "duplicate argument lost equality");
        range_is(state, 0, 0, 10);
        auto constants = state.remap({Operand::literal(minimum), Operand::literal(maximum)});
        equal(*constants, point({minimum, maximum}));
    });
    add("widen/bottom_and_stability", [] {
        auto state = box({{-2, 3}, {4, 8}});
        const auto before = state;
        state.widen_with(before); equal(state, before);
        state.widen_with(OctagonState(2, true)); equal(state, before);
        OctagonState bottom(2, true); bottom.widen_with(state); equal(bottom, state);
    });
    add("widen/raw_basis_survives_closed_views", [] {
        auto shape = [](int upper_x, int upper_y) {
            auto state = box({{minimum, upper_x}, {minimum, upper_y}});
            state.constrain(0, 1, 1, -1, 1);
            state.constrain(0, -1, 1, 1, 1);
            return state;
        };
        auto state = shape(0, 0);
        auto candidate = state;
        candidate.join_with(shape(1, 0));
        state.widen_with(candidate);
        require(candidate.leq(state), "first widening lost input");
        candidate = state;
        candidate.join_with(shape(1, 1));
        state.widen_with(candidate);
        require(candidate.leq(state), "second widening lost input");
        const auto stable = state.clone();
        for (int i = 2; i <= 40; ++i) {
            auto incoming = state.clone();
            incoming->join_with(shape(i, i));
            state.widen_with(*incoming);
            require(incoming->leq(state), "widening lost candidate");
            equal(state, *stable);
        }
        bound_is(state, 0, 2, 1); bound_is(state, 2, 0, 1);
    });

    const std::vector<std::pair<std::string, std::set<int>>> programs = {
        {"abort;", {1}},
        {"halt; abort;", {}},
        {"let x=0; input(x); if(x<0){abort;} else {abort;}", {1, 2}},
        {"let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=x+3; if(y-x!=3){abort;}", {}},
        {"let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=x+3; if(y-x==3){abort;}", {1}},
        {"let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=10-x; if(x+y!=10){abort;}", {}},
        {"let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=10-x; if(x+y==10){abort;}", {1}},
        {"let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=-x; if(-x-y!=0){abort;}", {}},
        {"let x=0; let y=0; while(x<10){x=x+1; y=y+1;} if(x!=y){abort;}", {}},
        {"let x=0; while(x>=0){x=x+1;} abort;", {}},
        {"while(1==1){} abort;", {}},
        {"let x=0; input(x); if(x==0){x=1;} else{x=3;} if(x==2){abort;}", {1}},
        {"let x=2147483647; x=x+1; abort;", {}},
    };
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto program = programs[i];
        add("solver/program_" + std::to_string(i), [program] {
            expect("fn main(){" + program.first + "}", program.second);
        });
    }
    add("solver/intra_call_boundaries", [] {
        expect("fn f(v){if(v!=0){abort;} return 0;} fn main(){let r=f(0); if(r!=0){abort;}}", {1, 2});
        expect("fn unused(){abort;} fn main(){halt;}", {});
        expect("fn f(){halt;} fn main(){let r=f(); abort;}", {1});
    });
    add("solver/call_preserves_caller_relation", [] {
        expect("fn f(){return 0;} fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;}"
               "let y=10-x; let r=f(); if(x+y!=10){abort;} if(r!=0){abort;}}", {2});
    });
    add("solver/widening_and_determinism", [] {
        const std::string source = "fn main(){let x=0; let y=1; while(x<100){x=x+1;y=y+1;} if(y-x!=1){abort;}}";
        AnalysisOptions options; options.widen_delay = 0;
        const auto first = analyze(source, options), second = analyze(source, options);
        require(first.status == AnalysisStatus::Success && first.may_abort.empty(), "relational loop did not converge");
        require(first.stats.widenings > 0 && first.stats.iterations < 1000, "widening did not accelerate convergence");
        require(first.may_abort == second.may_abort && first.stats.iterations == second.stats.iterations &&
                first.stats.widenings == second.stats.widenings, "non-deterministic octagon analysis");
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
        try { test.run(); ++passed; std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { std::cout << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    if (filter == "--list") return 0;
    if (executed == 0) { std::cerr << "No matching octagon tests\n"; return 2; }
    std::cout << "Octagon unit tests: " << executed << ", passed: " << passed << ", failed: " << executed - passed << '\n';
    return passed == executed ? 0 : 1;
}
