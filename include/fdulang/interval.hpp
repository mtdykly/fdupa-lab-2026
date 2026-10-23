#ifndef FDULANG_INTERVAL_HPP
#define FDULANG_INTERVAL_HPP

#include "fdulang/domain.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>

namespace fdulang {

constexpr std::int64_t integer_min = std::numeric_limits<std::int32_t>::min();
constexpr std::int64_t integer_max = std::numeric_limits<std::int32_t>::max();

// All stored endpoints are within the language's int32 range. int64 is used
// for calculations; neither endpoint is an infinity sentinel.
struct Interval {
    std::int64_t lower = integer_min;
    std::int64_t upper = integer_max;
    bool empty() const { return lower > upper; }
    bool singleton() const { return lower == upper; }
};

// Teacher-provided boundary helpers. An empty interval uses lower > upper.
Interval clip_integer_range(std::int64_t lower, std::int64_t upper);
Interval intersect_ranges(const Interval& left, const Interval& right);
bool range_subset(const Interval& left, const Interval& right);

// Teacher support: normalize a unit affine guard to variable <op> bound.
// Constants and expressions involving several variables are not unit guards.
struct UnaryGuard {
    bool valid = false;
    int variable = -1;
    CompareOp comparison = CompareOp::Equal;
    std::int64_t bound = 0;
};
UnaryGuard normalize_unary_guard(const LinearExpr& expression, CompareOp comparison);
bool compare_constants(std::int64_t left, CompareOp comparison, std::int64_t right);

class IntervalState final : public AbstractState {
public:
    explicit IntervalState(std::size_t variables, bool bottom = false);
    std::unique_ptr<AbstractState> clone() const override;
    bool is_bottom() const override { return bottom_; }
    bool leq(const AbstractState& other) const override;
    void join_with(const AbstractState& other) override;
    void widen_with(const AbstractState& other) override;
    void assign(int target, NodeKind op, const Operand& left,
                const Operand& right) override;
    void assume(const Node& branch, bool truth) override;
    void forget(int variable) override;
    std::unique_ptr<AbstractState> remap(
        const std::vector<Operand>& values) const override;
    std::unique_ptr<AbstractState> embed(
        std::size_t dimensions, const std::vector<int>& destinations) const override;
    void assign_return(int target, const AbstractState& scalar) override;

    std::size_t size() const { return values_.size(); }
    // Diagnostic/test access. Bottom returns an empty interval for every slot.
    Interval value(int variable) const;

private:
    std::vector<Interval> values_;
    bool bottom_ = false;

    const IntervalState& compatible(const AbstractState& other) const;
    std::size_t index(int variable) const;
    Interval read(const Operand& operand) const;
    void write(int variable, const Interval& value);
    void set_bottom();
};

std::unique_ptr<AbstractState> make_interval_top(std::size_t variables);
std::unique_ptr<AbstractState> make_interval_bottom(std::size_t variables);

} // namespace fdulang
#endif
