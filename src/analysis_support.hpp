#ifndef FDULANG_ANALYSIS_SUPPORT_HPP
#define FDULANG_ANALYSIS_SUPPORT_HPP
#include "fdulang/ir.hpp"
#include <vector>
namespace fdulang { namespace analysis_detail {
std::vector<bool> widening_points(const FunctionCfg& function);
} }
#endif
