#include "fdulang/interval.hpp"
#include <algorithm>

namespace fdulang {

void IntervalState::join_with(const AbstractState& other) {
    const auto& rhs = compatible(other);
    // TODO LAB1-JOIN: Replace this conservative upper bound with the least
    // interval upper bound. Inputs have equal dimensions. Preserve bottom's
    // identity law; e.g. [1,2] join [4,5] must become [1,5].
    // Safe default: copy the first reachable state, otherwise forget values.
    if (rhs.bottom_ || rhs.leq(*this)) return;
    if (bottom_) { values_ = rhs.values_; bottom_ = false; return; }
    std::fill(values_.begin(), values_.end(), Interval{});
}

void IntervalState::widen_with(const AbstractState& other) {
    const auto& rhs = compatible(other);
    // TODO LAB1-WIDEN: Produce an upper bound that stabilizes for increasing
    // inputs. Keep stable bounds and send expanding bounds to the language
    // endpoints. Example: [0,1] widened with [0,2] becomes [0,INT32_MAX].
    // Safe default: after the first reachable state, any expansion gives top.
    if (rhs.bottom_ || rhs.leq(*this)) return;
    if (bottom_) { values_ = rhs.values_; bottom_ = false; return; }
    std::fill(values_.begin(), values_.end(), Interval{});
}

void IntervalState::assign(int target, NodeKind op, const Operand& left,
                           const Operand& right) {
    // TODO LAB1-ASSIGN: Handle Copy/Add/Sub/Neg. Evaluate operands in the old
    // state before updating target, including x=x+1 and x=x-x. Use int64 raw
    // endpoints and write() to keep all normally completing int32 results.
    // Example: [2,4]+[1,3] produces [3,7]. Preserve bottom.
    // Safe default: forget the target; leave other slots and bottom unchanged.
    (void)op; (void)left; (void)right;
    forget(target);
}

void IntervalState::assume(const Node& branch, bool truth) {
    // TODO LAB1-ASSUME: Keep every represented execution satisfying the chosen
    // branch. Negate the comparison for false edges. Handle strict integer
    // bounds, equality/disequality, identical operands, and optional affine
    // metadata via normalize_unary_guard(). Example: [0,10] with x<3 becomes
    // [0,2]. An impossible guard becomes bottom; unsupported relational guards
    // may remain unchanged.
    // Safe default: retain the entire input state, including bottom.
    (void)branch; (void)truth;
}

} // namespace fdulang
