#ifndef FDULANG_OCTAGON_HPP
#define FDULANG_OCTAGON_HPP

#include "fdulang/interval.hpp"
#include <stdexcept>

namespace fdulang {

// Infinity is tagged, never an integer sentinel used in arithmetic.
struct DbmBound {
    bool infinite = true;
    std::int64_t value = 0;
    static DbmBound finite(std::int64_t value) { return {false, value}; }
};
bool bound_leq(const DbmBound& left, const DbmBound& right);
DbmBound bound_min(const DbmBound& left, const DbmBound& right);
DbmBound bound_max(const DbmBound& left, const DbmBound& right);
std::int64_t floor_half(std::int64_t value);

class OctagonResourceLimit : public std::runtime_error {
public:
    explicit OctagonResourceLimit(const std::string& message) : std::runtime_error(message) {}
};

class OctagonState final : public AbstractState {
public:
    // Capacity includes compiler temporaries. Exceeding it is a tool resource
    // failure, never bottom or a successful safety result.
    static constexpr std::size_t max_variables = 128;
    explicit OctagonState(std::size_t variables, bool bottom = false);
    std::unique_ptr<AbstractState> clone() const override;
    bool is_bottom() const override { return bottom_; }
    bool leq(const AbstractState& other) const override;
    void join_with(const AbstractState& other) override; // Lab2 task
    void widen_with(const AbstractState& other) override; // Teacher support
    void assign(int target, NodeKind op, const Operand& left,
                const Operand& right) override; // Lab2 task
    void assume(const Node& branch, bool truth) override; // Lab2 task
    void forget(int variable) override; // Lab2 task
    std::unique_ptr<AbstractState> remap(const std::vector<Operand>& values) const override;
    std::unique_ptr<AbstractState> embed(
        std::size_t dimensions, const std::vector<int>& destinations) const override;
    void assign_return(int target, const AbstractState& scalar) override;

    // Add sx*x + sy*y <= c. sx/sy are +/-1; y=-1 denotes sx*x <= c.
    // x==y is allowed. Leaves an integer-tight-closed state. Lab2 task.
    void constrain(int x, int sx, int y, int sy, std::int64_t c);
    std::size_t size() const { return variables_; }
    Interval value(int variable) const;
    // DBM convention: entry(p,q) bounds signed(p)-signed(q), with
    // signed(2*x)=+x, signed(2*x+1)=-x. These accessors are for diagnostics/tests.
    DbmBound difference_bound(std::size_t p, std::size_t q) const;
    bool contains(const std::vector<std::int32_t>& point) const;

private:
    std::size_t variables_;
    std::vector<DbmBound> dbm_;
    bool bottom_ = false;
    // Widening history is syntactic and MUST NOT be replaced by the closed view.
    std::vector<DbmBound> widening_basis_;

    const OctagonState& compatible(const AbstractState& other) const;
    std::size_t index(int variable) const;
    std::size_t width() const { return variables_ * 2; }
    std::size_t cell(std::size_t p, std::size_t q) const { return p * width() + q; }
    void reset_widening();
    void set_bottom();
    void normalize(); // Floyd-Warshall, integer tightening and strong closure.
    void add_difference(std::size_t p, std::size_t q, std::int64_t bound);
    void add_interval(int variable, const Interval& interval); // Intersect, not assign.
    // Teacher support: normalize E <, <=, >, >= 0, then call constrain.
    // Unrepresentable expressions are ignored; equality/disequality use Lab2.
    void assume_inequality(const LinearExpr& expression, CompareOp comparison);
    Interval read(const Operand& operand) const;
    void affine_image(int target, int source, int sign, std::int64_t offset);
};

std::unique_ptr<AbstractState> make_octagon_top(std::size_t variables);
std::unique_ptr<AbstractState> make_octagon_bottom(std::size_t variables);

} // namespace fdulang
#endif
