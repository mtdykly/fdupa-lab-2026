#include "fdulang/runtime.hpp"

#include <istream>
#include <stdexcept>
#include <vector>

namespace fdulang {
namespace {

bool read_integer(std::istream& input, std::int32_t& value, std::string& code) {
    std::string token;
    if (!(input >> token)) {
        code = input.bad() || !input.eof() ? "input_failure" : "input_eof";
        return false;
    }
    const bool negative = token[0] == '-';
    const std::size_t first_digit = token[0] == '+' || negative ? 1 : 0;
    // Check the whole token before its range, so even a long "999...abc" is
    // an invalid integer rather than a numeric overflow.
    if (first_digit == token.size() ||
        token.find_first_not_of("0123456789", first_digit) != std::string::npos) {
        code = "invalid_input";
        return false;
    }
    const std::int64_t limit = negative ? 2147483648LL : 2147483647LL;
    std::int64_t magnitude = 0;
    for (std::size_t i = first_digit; i < token.size(); ++i) {
        const int digit = token[i] - '0';
        if (magnitude > (limit - digit) / 10) {
            code = "input_out_of_range";
            return false;
        }
        magnitude = magnitude * 10 + digit;
    }
    value = static_cast<std::int32_t>(negative ? -magnitude : magnitude);
    return true;
}

struct Frame {
    int function = -1;
    int pc = -1;
    int return_target = -1;
    std::vector<std::int32_t> values;
};

Frame make_frame(const ProgramCfg& program, int function, int return_target,
                 const std::vector<std::int32_t>& arguments) {
    const FunctionCfg& cfg = program.functions.at(static_cast<std::size_t>(function));
    Frame frame;
    frame.function = function;
    frame.pc = cfg.entry;
    frame.return_target = return_target;
    frame.values.resize(cfg.variables.size(), 0);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const auto slot = static_cast<std::size_t>(cfg.parameters.at(i));
        frame.values.at(slot) = arguments[i];
    }
    return frame;
}

std::int32_t read(const Frame& frame, const Operand& operand) {
    if (operand.is_constant) return operand.constant;
    return frame.values.at(static_cast<std::size_t>(operand.variable));
}

void write(Frame& frame, int target, std::int32_t value) {
    frame.values.at(static_cast<std::size_t>(target)) = value;
}

bool compare(std::int32_t left, CompareOp op, std::int32_t right) {
    switch (op) {
    case CompareOp::Less: return left < right;
    case CompareOp::LessEqual: return left <= right;
    case CompareOp::Greater: return left > right;
    case CompareOp::GreaterEqual: return left >= right;
    case CompareOp::Equal: return left == right;
    case CompareOp::NotEqual: return left != right;
    }
    throw std::logic_error("unknown comparison");
}

} // namespace

const char* status_name(ExecutionStatus status) {
    switch (status) {
    case ExecutionStatus::Normal: return "normal";
    case ExecutionStatus::Abort: return "abort";
    case ExecutionStatus::RuntimeError: return "runtime_error";
    case ExecutionStatus::ResourceLimit: return "resource_limit";
    }
    return "internal_error";
}

ExecutionResult execute(const ProgramCfg& program, std::istream& input,
                        const ExecutionOptions& options) {
    std::vector<Frame> stack;
    stack.push_back(make_frame(program, program.entry_function, -1, {}));
    ExecutionResult result;
    while (!stack.empty()) {
        Frame& frame = stack.back();
        const FunctionCfg& function = program.functions.at(static_cast<std::size_t>(frame.function));
        const Node& node = function.nodes.at(static_cast<std::size_t>(frame.pc));
        result.span = node.span;
        if (options.max_steps != 0 && result.steps >= options.max_steps) {
            result.status = ExecutionStatus::ResourceLimit;
            result.code = "step_limit";
            result.message = "execution step limit reached";
            return result;
        }
        ++result.steps;

        switch (node.kind) {
        case NodeKind::Entry:
        case NodeKind::Nop:
            frame.pc = node.next;
            break;
        case NodeKind::Copy:
            write(frame, node.target, read(frame, node.left));
            frame.pc = node.next;
            break;
        case NodeKind::Add:
        case NodeKind::Sub:
        case NodeKind::Neg: {
            const std::int64_t left = read(frame, node.left);
            const std::int64_t right = node.kind == NodeKind::Neg ? 0 : read(frame, node.right);
            const std::int64_t value = node.kind == NodeKind::Add ? left + right
                : node.kind == NodeKind::Sub ? left - right : -left;
            if (value < -2147483648LL || value > 2147483647LL) {
                result.status = ExecutionStatus::RuntimeError;
                result.code = "integer_overflow";
                result.message = "integer arithmetic is outside the signed 32-bit range";
                return result;
            }
            write(frame, node.target, static_cast<std::int32_t>(value));
            frame.pc = node.next;
            break;
        }
        case NodeKind::Input: {
            std::int32_t value = 0;
            if (!read_integer(input, value, result.code)) {
                result.status = ExecutionStatus::RuntimeError;
                result.message = result.code == "input_eof" ? "expected an integer, but input ended"
                    : result.code == "input_failure" ? "failed to read the input stream"
                    : result.code == "input_out_of_range" ? "input is outside the signed 32-bit range"
                    : "input token must be a signed decimal integer";
                return result;
            }
            write(frame, node.target, value);
            frame.pc = node.next;
            break;
        }
        case NodeKind::Branch:
            frame.pc = compare(read(frame, node.left), node.comparison, read(frame, node.right))
                ? node.true_target : node.false_target;
            break;
        case NodeKind::Call: {
            if (options.max_call_depth != 0 && stack.size() >= options.max_call_depth) {
                result.status = ExecutionStatus::ResourceLimit;
                result.code = "call_depth_limit";
                result.message = "call depth limit reached";
                return result;
            }
            std::vector<std::int32_t> arguments;
            for (const Operand& operand : node.arguments) arguments.push_back(read(frame, operand));
            frame.pc = node.next;
            stack.push_back(make_frame(program, node.callee, node.target, arguments));
            break;
        }
        case NodeKind::Return: {
            const std::int32_t value = read(frame, node.left);
            const int target = frame.return_target;
            stack.pop_back();
            if (stack.empty()) {
                result.code = "main_return";
                return result;
            }
            write(stack.back(), target, value);
            break;
        }
        case NodeKind::Halt:
            result.code = "halt";
            return result;
        case NodeKind::Abort:
            result.status = ExecutionStatus::Abort;
            result.abort_id = node.abort_id;
            result.code = "explicit_abort";
            return result;
        }
    }
    throw std::logic_error("execution stack unexpectedly empty");
}

} // namespace fdulang
