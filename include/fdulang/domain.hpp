#ifndef FDULANG_DOMAIN_HPP
#define FDULANG_DOMAIN_HPP

#include "fdulang/ir.hpp"
#include <memory>
#include <vector>

namespace fdulang {

// A state describes all CFG slots in one function. Join/order operands must
// have the same domain and dimension. Bottom has no concrete executions.
// No operation is allowed to discard an execution that can complete normally.
class AbstractState {
public:
    virtual ~AbstractState() = default;
    virtual std::unique_ptr<AbstractState> clone() const = 0;
    virtual bool is_bottom() const = 0;
    virtual bool leq(const AbstractState& other) const = 0;

    // Mutate this state to an upper bound of both inputs. Widen additionally
    // guarantees stabilization when repeatedly applied to increasing inputs.
    virtual void join_with(const AbstractState& other) = 0;
    virtual void widen_with(const AbstractState& other) = 0;

    // op is Copy, Add, Sub, or Neg. Read every operand before overwriting target.
    // The result includes all int32 results of normally completing arithmetic.
    virtual void assign(int target, NodeKind op, const Operand& left,
                        const Operand& right) = 0;
    virtual void assume(const Node& branch, bool truth) = 0;
    virtual void forget(int variable) = 0;

    // Parallel binding/projection: result slot i receives values[i] evaluated
    // in the original state. Constants and repeated source slots are allowed.
    // A relational domain must retain expressible relations between outputs;
    // it must not implement binding by independently extracting intervals.
    virtual std::unique_ptr<AbstractState> remap(
        const std::vector<Operand>& values) const = 0;

    // Inject each source slot i into destinations[i] of a larger/different
    // namespace. Destinations must be distinct; remaining slots start at top.
    virtual std::unique_ptr<AbstractState> embed(
        std::size_t dimensions, const std::vector<int>& destinations) const = 0;

    // Restore the caller frame and bind a one-slot normal-return summary.
    // Other caller slots retain their constraints. This deliberately does not
    // recover relations between the returned value and the caller's arguments.
    virtual void assign_return(int target, const AbstractState& scalar) = 0;
};

} // namespace fdulang
#endif
