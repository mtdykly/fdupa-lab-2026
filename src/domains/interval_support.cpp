#include "fdulang/interval.hpp"
#include <algorithm>
#include <cassert>
#include <stdexcept>

namespace fdulang {

Interval clip_integer_range(std::int64_t lower, std::int64_t upper) {
    return {std::max(lower, integer_min), std::min(upper, integer_max)};
}

Interval intersect_ranges(const Interval& left, const Interval& right) {
    return {std::max(left.lower, right.lower), std::min(left.upper, right.upper)};
}

bool range_subset(const Interval& left, const Interval& right) {
    if (left.empty()) return true;
    return !right.empty() && left.lower >= right.lower && left.upper <= right.upper;
}

bool compare_constants(std::int64_t left, CompareOp comparison, std::int64_t right) {
    switch (comparison) {
    case CompareOp::Less: return left < right;
    case CompareOp::LessEqual: return left <= right;
    case CompareOp::Greater: return left > right;
    case CompareOp::GreaterEqual: return left >= right;
    case CompareOp::Equal: return left == right;
    case CompareOp::NotEqual: return left != right;
    }
    assert(false);
    return false;
}

UnaryGuard normalize_unary_guard(const LinearExpr& expression, CompareOp comparison) {
    if (!expression.valid || expression.coefficients.size() != 1) return {};
    const auto term = *expression.coefficients.begin();
    if (term.second != 1 && term.second != -1) return {};
    UnaryGuard result{true, term.first, comparison, expression.constant};
    if (term.second == 1) {
        // -INT64_MIN is outside int32. Saturating this comparison bound keeps
        // its meaning for every legal FDULang value without overflowing C++.
        result.bound = expression.constant == std::numeric_limits<std::int64_t>::min()
            ? std::numeric_limits<std::int64_t>::max() : -expression.constant;
    } else {
        switch (comparison) {
        case CompareOp::Less: result.comparison = CompareOp::Greater; break;
        case CompareOp::LessEqual: result.comparison = CompareOp::GreaterEqual; break;
        case CompareOp::Greater: result.comparison = CompareOp::Less; break;
        case CompareOp::GreaterEqual: result.comparison = CompareOp::LessEqual; break;
        case CompareOp::Equal: case CompareOp::NotEqual: break;
        }
    }
    return result;
}

IntervalState::IntervalState(std::size_t variables, bool bottom)
    : values_(variables), bottom_(bottom) {}

std::unique_ptr<AbstractState> IntervalState::clone() const {
    return std::make_unique<IntervalState>(*this);
}

const IntervalState& IntervalState::compatible(const AbstractState& other) const {
    const auto* interval = dynamic_cast<const IntervalState*>(&other);
    if (!interval || interval->size() != size())
        throw std::invalid_argument("incompatible abstract states");
    return *interval;
}

std::size_t IntervalState::index(int variable) const {
    if (variable < 0 || static_cast<std::size_t>(variable) >= values_.size())
        throw std::out_of_range("invalid interval variable");
    return static_cast<std::size_t>(variable);
}

Interval IntervalState::value(int variable) const {
    const auto slot = index(variable);
    return bottom_ ? Interval{1, 0} : values_[slot];
}

Interval IntervalState::read(const Operand& operand) const {
    if (bottom_) return {1, 0};
    if (operand.is_constant) return {operand.constant, operand.constant};
    return value(operand.variable);
}

void IntervalState::set_bottom() {
    bottom_ = true;
}

void IntervalState::write(int variable, const Interval& value) {
    const auto slot = index(variable);
    if (bottom_) return;
    const auto clipped = clip_integer_range(value.lower, value.upper);
    if (clipped.empty()) set_bottom();
    else values_[slot] = clipped;
}

bool IntervalState::leq(const AbstractState& other) const {
    const auto& rhs = compatible(other);
    if (bottom_) return true;
    if (rhs.bottom_) return false;
    for (std::size_t i = 0; i < size(); ++i)
        if (!range_subset(values_[i], rhs.values_[i])) return false;
    return true;
}

void IntervalState::forget(int variable) {
    const auto slot = index(variable);
    if (!bottom_) values_[slot] = Interval{};
}

std::unique_ptr<AbstractState> IntervalState::remap(
    const std::vector<Operand>& operands) const {
    auto result = std::make_unique<IntervalState>(operands.size(), bottom_);
    if (!bottom_)
        for (std::size_t i = 0; i < operands.size(); ++i)
            result->values_[i] = read(operands[i]);
    return result;
}

std::unique_ptr<AbstractState> make_interval_top(std::size_t variables) {
    return std::make_unique<IntervalState>(variables);
}

std::unique_ptr<AbstractState> IntervalState::embed(
    std::size_t dimensions, const std::vector<int>& destinations) const {
    if (destinations.size() != size()) throw std::invalid_argument("embedding dimension mismatch");
    auto result = std::make_unique<IntervalState>(dimensions, bottom_);
    std::vector<bool> used(dimensions, false);
    for (std::size_t i = 0; i < size(); ++i) {
        const auto target = result->index(destinations[i]);
        if (used[target]) throw std::invalid_argument("duplicate embedding destination");
        used[target] = true;
        if (!bottom_) result->values_[target] = values_[i];
    }
    return result;
}

void IntervalState::assign_return(int target, const AbstractState& scalar) {
    index(target);
    const auto* value = dynamic_cast<const IntervalState*>(&scalar);
    if (!value || value->size() != 1) throw std::invalid_argument("return summary must have one slot in the same domain");
    const auto range = value->value(0); // Read before changing a possibly aliased state.
    if (range.empty()) set_bottom();
    else write(target, range);
}

std::unique_ptr<AbstractState> make_interval_bottom(std::size_t variables) {
    return std::make_unique<IntervalState>(variables, true);
}

} // namespace fdulang
