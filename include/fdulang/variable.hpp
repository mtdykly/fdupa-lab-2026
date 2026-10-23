#ifndef FDULANG_VARIABLE_HPP
#define FDULANG_VARIABLE_HPP

#include "fdulang/source.hpp"

namespace fdulang {
struct Variable {
    std::string name;
    SourceSpan span;
    bool parameter = false;
    bool temporary = false;
};
} // namespace fdulang
#endif
