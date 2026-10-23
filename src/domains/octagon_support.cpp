#include "fdulang/octagon.hpp"
#include <algorithm>
#include <cassert>
#include <limits>

namespace fdulang {
namespace {

std::int64_t signed_lower(std::size_t p) {
    return p % 2 == 0 ? integer_min : -integer_max;
}
std::int64_t signed_upper(std::size_t p) {
    return p % 2 == 0 ? integer_max : -integer_min;
}
std::int64_t minimum_difference(std::size_t p, std::size_t q) {
    return p == q ? 0 : signed_lower(p) - signed_upper(q);
}
std::int64_t maximum_difference(std::size_t p, std::size_t q) {
    return p == q ? 0 : signed_upper(p) - signed_lower(q);
}

bool octagonal_shape(const LinearExpr& expression,
                     std::vector<std::pair<int, std::int64_t>>& terms) {
    if (!expression.valid) return false;
    for (const auto& term : expression.coefficients)
        if (term.second != 0) terms.push_back(term);
    if (terms.empty()) return true;
    if (terms.size() == 1)
        return terms[0].second == 1 || terms[0].second == -1 ||
               terms[0].second == 2 || terms[0].second == -2;
    return terms.size() == 2 &&
           (terms[0].second == 1 || terms[0].second == -1) &&
           (terms[1].second == 1 || terms[1].second == -1);
}

} // namespace

bool bound_leq(const DbmBound& left, const DbmBound& right) {
    return right.infinite || (!left.infinite && left.value <= right.value);
}
DbmBound bound_min(const DbmBound& left, const DbmBound& right) {
    return bound_leq(left, right) ? left : right;
}
DbmBound bound_max(const DbmBound& left, const DbmBound& right) {
    return bound_leq(left, right) ? right : left;
}
std::int64_t floor_half(std::int64_t value) {
    // C++ division truncates towards zero, whereas integer tightening needs floor.
    return value / 2 - (value < 0 && value % 2 != 0 ? 1 : 0);
}

OctagonState::OctagonState(std::size_t variables, bool bottom)
    : variables_(variables), bottom_(bottom) {
    if (variables > max_variables)
        throw OctagonResourceLimit("octagon domain supports at most 128 CFG slots per function (including temporaries)");
    // The solver initially allocates bottom for every CFG node. A bottom state
    // has no matrix until it first receives a reachable state.
    if (bottom_) return;
    dbm_.resize(width() * width());
    // The Cartesian product of int32 bounds is already tight-closed.
    for (std::size_t p = 0; p < width(); ++p)
        for (std::size_t q = 0; q < width(); ++q)
            dbm_[cell(p, q)] = DbmBound::finite(maximum_difference(p, q));
}

std::unique_ptr<AbstractState> OctagonState::clone() const {
    return std::make_unique<OctagonState>(*this);
}
const OctagonState& OctagonState::compatible(const AbstractState& other) const {
    const auto* rhs = dynamic_cast<const OctagonState*>(&other);
    if (!rhs || rhs->size() != size()) throw std::invalid_argument("incompatible octagon states");
    return *rhs;
}
std::size_t OctagonState::index(int variable) const {
    if (variable < 0 || static_cast<std::size_t>(variable) >= variables_)
        throw std::out_of_range("invalid octagon variable");
    return static_cast<std::size_t>(variable);
}
void OctagonState::reset_widening() { widening_basis_.clear(); }
void OctagonState::set_bottom() { bottom_ = true; reset_widening(); }

DbmBound OctagonState::difference_bound(std::size_t p, std::size_t q) const {
    if (p >= width() || q >= width()) throw std::out_of_range("invalid signed variable");
    if (bottom_) throw std::logic_error("bottom has no finite canonical DBM");
    return dbm_[cell(p, q)];
}
Interval OctagonState::value(int variable) const {
    const auto p = index(variable) * 2;
    if (bottom_) return {1, 0};
    return {-floor_half(dbm_[cell(p ^ 1, p)].value),
             floor_half(dbm_[cell(p, p ^ 1)].value)};
}
Interval OctagonState::read(const Operand& operand) const {
    if (bottom_) return {1, 0};
    return operand.is_constant ? Interval{operand.constant, operand.constant} : value(operand.variable);
}
bool OctagonState::contains(const std::vector<std::int32_t>& point) const {
    if (point.size() != variables_) throw std::invalid_argument("point has wrong dimension");
    if (bottom_) return false;
    for (std::size_t p = 0; p < width(); ++p) {
        const std::int64_t left = (p % 2 == 0 ? 1 : -1) * static_cast<std::int64_t>(point[p / 2]);
        for (std::size_t q = 0; q < width(); ++q) {
            const std::int64_t right = (q % 2 == 0 ? 1 : -1) * static_cast<std::int64_t>(point[q / 2]);
            const auto& bound = dbm_[cell(p, q)];
            if (!bound.infinite && left - right > bound.value) return false;
        }
    }
    return true;
}
bool OctagonState::leq(const AbstractState& other) const {
    const auto& rhs = compatible(other);
    if (bottom_) return true;
    if (rhs.bottom_) return false;
    for (std::size_t i = 0; i < dbm_.size(); ++i)
        if (!bound_leq(dbm_[i], rhs.dbm_[i])) return false;
    return true;
}

void OctagonState::add_difference(std::size_t p, std::size_t q, std::int64_t bound) {
    if (p >= width() || q >= width()) throw std::out_of_range("invalid signed variable");
    if (bottom_) return;
    if (bound < minimum_difference(p, q)) { set_bottom(); return; }
    const auto finite = DbmBound::finite(bound);
    dbm_[cell(p, q)] = bound_min(dbm_[cell(p, q)], finite);
    dbm_[cell(q ^ 1, p ^ 1)] = bound_min(dbm_[cell(q ^ 1, p ^ 1)], finite);
}

void OctagonState::normalize() {
    if (bottom_) return;
    const std::size_t n = width();
    // Reinstate fixed language bounds in the semantic view only. They imply a
    // finite bound on every difference, so finite arithmetic below stays small.
    for (std::size_t p = 0; p < n; ++p)
        for (std::size_t q = 0; q < n; ++q) {
            auto bound = bound_min(dbm_[cell(p, q)], dbm_[cell(q ^ 1, p ^ 1)]);
            bound = bound_min(bound, DbmBound::finite(maximum_difference(p, q)));
            if (bound.value < minimum_difference(p, q)) { set_bottom(); return; }
            dbm_[cell(p, q)] = dbm_[cell(q ^ 1, p ^ 1)] = bound;
        }
    // Detect inconsistency immediately, before negative cycles could drive
    // intermediate bounds beyond int64. Every stored finite bound is at most
    // 2^32 in magnitude; adding two therefore cannot overflow int64.
    for (std::size_t k = 0; k < n; ++k)
        for (std::size_t p = 0; p < n; ++p)
            for (std::size_t q = 0; q < n; ++q) {
                const auto sum = dbm_[cell(p, k)].value + dbm_[cell(k, q)].value;
                if (sum < dbm_[cell(p, q)].value) {
                    if (sum < minimum_difference(p, q)) { set_bottom(); return; }
                    dbm_[cell(p, q)] = DbmBound::finite(sum);
                }
            }
    // Tight closure for integer octagons: snapshot both unary halves before
    // strengthening. A rationally feasible octagon can have no integer point.
    std::vector<std::int64_t> half(n);
    for (std::size_t p = 0; p < n; ++p) half[p] = floor_half(dbm_[cell(p, p ^ 1)].value);
    for (std::size_t p = 0; p < n; ++p)
        if (half[p] + half[p ^ 1] < 0) { set_bottom(); return; }
    for (std::size_t p = 0; p < n; ++p)
        for (std::size_t q = 0; q < n; ++q)
            dbm_[cell(p, q)] = bound_min(dbm_[cell(p, q)], DbmBound::finite(half[p] + half[q ^ 1]));
}

void OctagonState::add_interval(int variable, const Interval& interval) {
    const auto p = index(variable) * 2;
    if (bottom_) return;
    const auto clipped = clip_integer_range(interval.lower, interval.upper);
    if (clipped.empty()) { set_bottom(); return; }
    add_difference(p, p ^ 1, 2 * clipped.upper);
    add_difference(p ^ 1, p, -2 * clipped.lower);
    normalize();
}

void OctagonState::assume_inequality(const LinearExpr& expression, CompareOp comparison) {
    assert(comparison != CompareOp::Equal && comparison != CompareOp::NotEqual);
    if (bottom_) return;
    std::vector<std::pair<int, std::int64_t>> terms;
    if (!octagonal_shape(expression, terms)) return;
    if (terms.empty()) {
        if (!compare_constants(expression.constant, comparison, 0)) set_bottom();
        return;
    }

    // At most two signed int32 values occur in a supported guard. Bounds far
    // outside their range can be capped before negation and strict-bound +/-1.
    constexpr std::int64_t limit = std::int64_t{1} << 40;
    const auto constant = std::max(-limit, std::min(limit, expression.constant));
    const bool negative = comparison == CompareOp::Greater || comparison == CompareOp::GreaterEqual;
    const bool strict = comparison == CompareOp::Less || comparison == CompareOp::Greater;
    const std::int64_t bound = (negative ? constant : -constant) - (strict ? 1 : 0);
    const int polarity = negative ? -1 : 1;
    const int sx = (terms[0].second > 0 ? 1 : -1) * polarity;
    if (terms.size() == 2) {
        const int sy = (terms[1].second > 0 ? 1 : -1) * polarity;
        constrain(terms[0].first, sx, terms[1].first, sy, bound);
    } else if (terms[0].second == 2 || terms[0].second == -2) {
        constrain(terms[0].first, sx, terms[0].first, sx, bound);
    } else {
        constrain(terms[0].first, sx, -1, 1, bound);
    }
}

void OctagonState::affine_image(int target, int source, int sign, std::int64_t offset) {
    const auto target_index = index(target), source_index = index(source);
    if (sign != 1 && sign != -1) throw std::invalid_argument("affine sign must be +/-1");
    if (bottom_) return;
    const auto source_min = sign == 1 ? integer_min : -integer_max;
    const auto source_max = sign == 1 ? integer_max : -integer_min;
    if (offset < integer_min - source_max || offset > integer_max - source_min) {
        set_bottom(); // No int32 result for any int32 source value.
        return;
    }
    reset_widening();
    const auto old = dbm_;
    std::vector<std::size_t> mapping(width());
    std::vector<std::int64_t> shifts(width(), 0);
    for (std::size_t p = 0; p < width(); ++p) {
        mapping[p] = p;
        if (p / 2 == target_index) {
            const int polarity = p % 2 == 0 ? 1 : -1;
            mapping[p] = 2 * source_index + (polarity * sign == 1 ? 0 : 1);
            shifts[p] = polarity * offset;
        }
    }
    for (std::size_t p = 0; p < width(); ++p)
        for (std::size_t q = 0; q < width(); ++q) {
            const auto& bound = old[cell(mapping[p], mapping[q])];
            dbm_[cell(p, q)] = bound.infinite ? bound :
                DbmBound::finite(bound.value + shifts[p] - shifts[q]);
        }
    normalize();
}

void OctagonState::widen_with(const AbstractState& other) {
    const auto& rhs = compatible(other);
    if (rhs.bottom_) return;
    if (bottom_) {
        *this = rhs;
        reset_widening();
        return;
    }
    if (widening_basis_.empty()) widening_basis_ = dbm_;
    // Do not close this matrix: an entry may be dropped only once. This finite
    // sequence is the termination argument, independently of the closed view.
    for (std::size_t i = 0; i < widening_basis_.size(); ++i)
        if (!bound_leq(rhs.dbm_[i], widening_basis_[i])) widening_basis_[i] = DbmBound{};
    dbm_ = widening_basis_;
    normalize(); // Leaves the raw widening basis untouched for the next call.
}

std::unique_ptr<AbstractState> OctagonState::remap(const std::vector<Operand>& values) const {
    auto result = std::make_unique<OctagonState>(values.size(), bottom_);
    if (bottom_) return result;
    // Project/rename all outputs from the SAME old state. Repeated source slots
    // use old diagonal zero, and hence retain equality between output slots.
    for (std::size_t p = 0; p < result->width(); ++p) {
        const auto& left = values[p / 2];
        const int left_sign = p % 2 == 0 ? 1 : -1;
        if (!left.is_constant) index(left.variable);
        for (std::size_t q = 0; q < result->width(); ++q) {
            const auto& right = values[q / 2];
            const int right_sign = q % 2 == 0 ? 1 : -1;
            if (!right.is_constant) index(right.variable);
            std::int64_t bound;
            if (!left.is_constant && !right.is_constant) {
                const auto a = 2 * static_cast<std::size_t>(left.variable) + (left_sign == 1 ? 0 : 1);
                const auto b = 2 * static_cast<std::size_t>(right.variable) + (right_sign == 1 ? 0 : 1);
                bound = dbm_[cell(a, b)].value;
            } else if (left.is_constant && right.is_constant) {
                bound = left_sign * static_cast<std::int64_t>(left.constant) -
                        right_sign * static_cast<std::int64_t>(right.constant);
            } else if (left.is_constant) {
                const auto range = read(right);
                const auto lower = right_sign == 1 ? range.lower : -range.upper;
                bound = left_sign * static_cast<std::int64_t>(left.constant) - lower;
            } else {
                const auto range = read(left);
                const auto upper = left_sign == 1 ? range.upper : -range.lower;
                bound = upper - right_sign * static_cast<std::int64_t>(right.constant);
            }
            result->dbm_[result->cell(p, q)] = DbmBound::finite(bound);
        }
    }
    result->normalize();
    return result;
}

std::unique_ptr<AbstractState> make_octagon_top(std::size_t variables) {
    return std::make_unique<OctagonState>(variables);
}

std::unique_ptr<AbstractState> OctagonState::embed(
    std::size_t dimensions, const std::vector<int>& destinations) const {
    if (destinations.size() != size()) throw std::invalid_argument("embedding dimension mismatch");
    auto result = std::make_unique<OctagonState>(dimensions, bottom_);
    std::vector<bool> used(dimensions, false);
    for (const int target : destinations) {
        const auto slot = result->index(target);
        if (used[slot]) throw std::invalid_argument("duplicate embedding destination");
        used[slot] = true;
    }
    if (bottom_) return result;
    for (std::size_t p = 0; p < width(); ++p)
        for (std::size_t q = 0; q < width(); ++q) {
            const auto a = 2 * static_cast<std::size_t>(destinations[p / 2]) + p % 2;
            const auto b = 2 * static_cast<std::size_t>(destinations[q / 2]) + q % 2;
            result->dbm_[result->cell(a, b)] = dbm_[cell(p, q)];
        }
    result->normalize();
    return result;
}

void OctagonState::assign_return(int target, const AbstractState& scalar) {
    index(target);
    const auto* value = dynamic_cast<const OctagonState*>(&scalar);
    if (!value || value->size() != 1) throw std::invalid_argument("return summary must have one slot in the same domain");
    const auto range = value->value(0);
    if (range.empty()) { set_bottom(); return; }
    forget(target);
    add_interval(target, range);
}
std::unique_ptr<AbstractState> make_octagon_bottom(std::size_t variables) {
    return std::make_unique<OctagonState>(variables, true);
}

} // namespace fdulang
