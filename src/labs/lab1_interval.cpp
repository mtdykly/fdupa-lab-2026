#include "fdulang/interval.hpp"
#include <algorithm>
#include <stdexcept>

namespace fdulang {

void IntervalState::join_with(const AbstractState& other) {
    const auto& rhs = compatible(other);
    // TODO LAB1-JOIN: Replace this conservative upper bound with the least
    // interval upper bound. Inputs have equal dimensions. Preserve bottom's
    // identity law; e.g. [1,2] join [4,5] must become [1,5].
    // Safe default: copy the first reachable state, otherwise forget values.
    if (rhs.bottom_) {
        return;
    }

    if (bottom_) {
        values_ = rhs.values_;
        bottom_ = false;
        return;
    }
    
    for (std::size_t i = 0; i < values_.size(); ++i) {
        values_[i].lower =
            std::min(values_[i].lower, rhs.values_[i].lower);

        values_[i].upper =
            std::max(values_[i].upper, rhs.values_[i].upper);
    }
}

void IntervalState::widen_with(const AbstractState& other) {
    const auto& rhs = compatible(other);
    // TODO LAB1-WIDEN: Produce an upper bound that stabilizes for increasing
    // inputs. Keep stable bounds and send expanding bounds to the language
    // endpoints. Example: [0,1] widened with [0,2] becomes [0,INT32_MAX].
    // Safe default: after the first reachable state, any expansion gives top.
    
    if (rhs.bottom_) {
        return;
    }

    if (bottom_) {
        values_ = rhs.values_;
        bottom_ = false;
        return;
    }

    for (std::size_t i = 0; i < values_.size(); ++i) {
        if (rhs.values_[i].lower < values_[i].lower) {
            values_[i].lower = integer_min;
        }

        if (rhs.values_[i].upper > values_[i].upper) {
            values_[i].upper = integer_max;
        }
    }
}

void IntervalState::assign(int target, NodeKind op, const Operand& left,
                           const Operand& right) {
    // TODO LAB1-ASSIGN: Handle Copy/Add/Sub/Neg. Evaluate operands in the old
    // state before updating target, including x=x+1 and x=x-x. Use int64 raw
    // endpoints and write() to keep all normally completing int32 results.
    // Example: [2,4]+[1,3] produces [3,7]. Preserve bottom.
    // Safe default: forget the target; leave other slots and bottom unchanged.

    if (bottom_) {
        return;
    }
    const auto lhs = read(left);
    Interval result;
    switch (op) {
        case NodeKind::Copy:
            result = lhs;
            break;
        
        case NodeKind::Neg:
            result = {-lhs.upper, -lhs.lower};
            break;

        case NodeKind::Add: {
            const auto rhs = read(right);
                result = {
                    lhs.lower + rhs.lower,
                    lhs.upper + rhs.upper
                };
                break;
        }

        case NodeKind::Sub: {
            if (!left.is_constant && !right.is_constant && left.variable == right.variable) {
                result = {0, 0};
            } else {
                const auto rhs = read(right);
                result = {
                    lhs.lower - rhs.upper,
                    lhs.upper - rhs.lower
                };
            }
            break;
        }

    default:
        throw std::logic_error("unsupported interval assignment");
    }
    write(target, result);
}

void IntervalState::assume(const Node& branch, bool truth) {
    // TODO LAB1-ASSUME: Keep every represented execution satisfying the chosen
    // branch. Negate the comparison for false edges. Handle strict integer
    // bounds, equality/disequality, identical operands, and optional affine
    // metadata via normalize_unary_guard(). Example: [0,10] with x<3 becomes
    // [0,2]. An impossible guard becomes bottom; unsupported relational guards
    // may remain unchanged.
    // Safe default: retain the entire input state, including bottom.
    
    if (bottom_) return;

    const auto op = truth ? branch.comparison : negate_compare(branch.comparison);

    auto filter = [](Interval& a, Interval& b, CompareOp cmp) {
        const auto old_a = a;
        const auto old_b = b;

        switch (cmp) {
            case CompareOp::Less:
                if (old_a.lower >= old_b.upper) return false;
                a.upper = std::min(old_a.upper, old_b.upper - 1);
                b.lower = std::max(old_b.lower, old_a.lower + 1);
                break;

            case CompareOp::LessEqual:
                a.upper = std::min(old_a.upper, old_b.upper);
                b.lower = std::max(old_b.lower, old_a.lower);
                break;

            case CompareOp::Greater:
                if (old_a.upper <= old_b.lower) return false;
                a.lower = std::max(old_a.lower, old_b.lower + 1);
                b.upper = std::min(old_b.upper, old_a.upper - 1);
                break;

            case CompareOp::GreaterEqual:
                a.lower = std::max(old_a.lower, old_b.lower);
                b.upper = std::min(old_b.upper, old_a.upper);
                break;

            case CompareOp::Equal:
                a = intersect_ranges(old_a, old_b);
                b = a;
                break;

            case CompareOp::NotEqual:
                if (old_b.singleton()) {
                    if (old_a.lower == old_b.lower) ++a.lower;
                    else if (old_a.upper == old_b.lower) --a.upper;
                }
                if (old_a.singleton()) {
                    if (old_b.lower == old_a.lower) ++b.lower;
                    else if (old_b.upper == old_a.lower) --b.upper;
                }
                break;
        }

        return !a.empty() && !b.empty();
    };

    const auto& left = branch.left;
    const auto& right = branch.right;

    if (!left.is_constant && !right.is_constant && left.variable == right.variable) {
        if (!compare_constants(0, op, 0)) {
            set_bottom();
            return;
        }
    } else {
        auto a = read(left);
        auto b = read(right);

        if (!filter(a, b, op)) {
            set_bottom();
            return;
        }

        if (!left.is_constant) write(left.variable, a);
        if (!right.is_constant) write(right.variable, b);
    }

    const auto& condition = branch.condition;
    if (!condition.valid) return;

    if (condition.coefficients.empty()) {
        if (!compare_constants(condition.constant, op, 0))
            set_bottom();
        return;
    }

    const auto guard = normalize_unary_guard(condition, op);
    if (!guard.valid) return;

    auto variable_range = read(Operand::slot(guard.variable));
    Interval bound_range{guard.bound, guard.bound};

    if (!filter(variable_range, bound_range, guard.comparison)) {
        set_bottom();
        return;
    }

    write(guard.variable, variable_range);
}

} // namespace fdulang
