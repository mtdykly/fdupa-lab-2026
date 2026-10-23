#ifndef FDULANG_RUNTIME_HPP
#define FDULANG_RUNTIME_HPP

#include "fdulang/ir.hpp"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace fdulang {

enum class ExecutionStatus { Normal, Abort, RuntimeError, ResourceLimit };
struct ExecutionOptions {
    std::uint64_t max_steps = 1000000;
    std::size_t max_call_depth = 10000;
};
struct ExecutionResult {
    ExecutionStatus status = ExecutionStatus::Normal;
    std::string code;
    std::string message;
    SourceSpan span;
    int abort_id = 0;
    std::uint64_t steps = 0;
};

const char* status_name(ExecutionStatus status);
// Zero disables the corresponding resource limit. Execution uses an explicit stack.
ExecutionResult execute(const ProgramCfg& program, std::istream& input,
                        const ExecutionOptions& options = ExecutionOptions{});

} // namespace fdulang
#endif
