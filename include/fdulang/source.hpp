#ifndef FDULANG_SOURCE_HPP
#define FDULANG_SOURCE_HPP

#include <cstddef>
#include <stdexcept>
#include <string>

namespace fdulang {

struct Position {
    std::size_t offset = 0;
    std::size_t line = 1;
    std::size_t column = 1;
};

struct SourceSpan {
    Position begin;
    Position end;
};

class Diagnostic : public std::runtime_error {
public:
    Diagnostic(const std::string& category, const SourceSpan& span,
               const std::string& message)
        : std::runtime_error(message), category(category), span(span) {}
    std::string category;
    SourceSpan span;
};

enum class CompareOp { Less, LessEqual, Greater, GreaterEqual, Equal, NotEqual };
inline const char* compare_text(CompareOp op) {
    switch (op) {
    case CompareOp::Less: return "<";
    case CompareOp::LessEqual: return "<=";
    case CompareOp::Greater: return ">";
    case CompareOp::GreaterEqual: return ">=";
    case CompareOp::Equal: return "==";
    case CompareOp::NotEqual: return "!=";
    }
    return "?";
}
inline CompareOp negate_compare(CompareOp op) {
    switch (op) {
    case CompareOp::Less: return CompareOp::GreaterEqual;
    case CompareOp::LessEqual: return CompareOp::Greater;
    case CompareOp::Greater: return CompareOp::LessEqual;
    case CompareOp::GreaterEqual: return CompareOp::Less;
    case CompareOp::Equal: return CompareOp::NotEqual;
    case CompareOp::NotEqual: return CompareOp::Equal;
    }
    return CompareOp::NotEqual;
}

struct StaticSite {
    int id = 0; // A/C identifiers are one based; function indices are zero based.
    int function = -1;
    SourceSpan span;
};

} // namespace fdulang
#endif
