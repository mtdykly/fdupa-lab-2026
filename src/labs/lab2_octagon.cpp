#include "fdulang/octagon.hpp"

namespace fdulang {

void OctagonState::constrain(int x, int sx, int y, int sy, std::int64_t c) {
    // TODO LAB2-CONSTRAIN: Encode sx*x+sy*y<=c using the two signed views of
    // each variable. y=-1 denotes a unary constraint. Account for coherence,
    // identical variables, integer bounds, and tight closure via normalize().
    // Example: x+y<=5 constrains (+x)-(-y) and (+y)-(-x).
    // Safe default: ignore the constraint, retaining every possible state.
    (void)x; (void)sx; (void)y; (void)sy; (void)c;
    reset_widening();
}

void OctagonState::forget(int variable) {
    // TODO LAB2-FORGET: Remove constraints involving both signed views of this
    // variable, retaining the closed consequences among all other variables.
    // Restore diagonal zero and normalize to add the language's int32 bounds.
    // Example: forgetting x from x=y and y=4 retains y=4 and makes x unknown.
    // Safe default: forget the whole reachable state, but preserve bottom.
    index(variable);
    reset_widening();
    if (!bottom_) *this = OctagonState(size());
}

void OctagonState::join_with(const AbstractState& other) {
    // TODO LAB2-JOIN: Compute the least octagonal upper bound of two tight-
    // closed states with equal dimensions. Preserve bottom's identity law.
    // Example: x-y<=1 joined with x-y<=3 must admit both sets of valuations.
    // Safe default: first reachable input is copied; any growth becomes top.
    const auto& rhs = compatible(other);
    reset_widening();
    if (rhs.bottom_ || rhs.leq(*this)) return;
    if (bottom_) { dbm_ = rhs.dbm_; bottom_ = false; return; }
    *this = OctagonState(size());
}

void OctagonState::assign(int target, NodeKind op, const Operand& left,
                          const Operand& right) {
    // TODO LAB2-ASSIGN: Evaluate old operands, then assign Copy/Add/Sub/Neg.
    // Preserve exact +/-source+constant relations with affine_image(), even
    // for x=x+1 or x=-x; use a safe interval fallback for general expressions.
    // Example: y=10-x should preserve x+y=10. Only int32 results may continue.
    // Safe default: forget target (the default forget conservatively loses all
    // relations); leave bottom unchanged.
    (void)op; (void)left; (void)right;
    forget(target);
}

void OctagonState::assume(const Node& branch, bool truth) {
    // TODO LAB2-ASSUME: Filter the chosen branch using signed octagonal guards
    // and safe fallbacks. Negate comparisons on false edges. Handle strict
    // integer bounds, == as two inequalities, and != as two branches joined.
    // assume_inequality() prepares <, <=, >, >= guards and calls constrain().
    // Example: after x+y=10, x+y!=10 must become bottom. Apply operand and
    // source-condition guards; the helper ignores unsupported metadata.
    // Safe default: retain the entire state, including bottom.
    (void)branch; (void)truth;
    reset_widening();
}

} // namespace fdulang
