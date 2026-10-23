#include "fdulang/lab3.hpp"

namespace fdulang {
CallString next_context(const CallString&, int, std::size_t) {
    // TODO(Lab3.1): retain the final k call sites, ordered oldest to newest.
    return {}; // Safe fallback: merge every invocation of each function.
}

std::unique_ptr<AbstractState> bind_call_arguments(
    const AbstractState& caller, const Node&, const FunctionCfg& callee,
    const DomainFactory& domain) {
    // TODO(Lab3.2): parallel argument binding, including relational information.
    return caller.is_bottom() ? domain.bottom(callee.variables.size()) : domain.top(callee.variables.size());
}

std::unique_ptr<AbstractState> join_call_entry(
    const AbstractState& current, const AbstractState& incoming,
    const DomainFactory& domain, std::size_t dimensions) {
    // TODO(Lab3.3): merge existing entry and new invocation with the domain join.
    return current.is_bottom() && incoming.is_bottom() ? domain.bottom(dimensions) : domain.top(dimensions);
}

std::unique_ptr<AbstractState> join_return_summary(
    const AbstractState& current, const AbstractState& incoming, const DomainFactory& domain) {
    // TODO(Lab3.4): merge all reachable normal-return values in this context.
    return current.is_bottom() && incoming.is_bottom() ? domain.bottom(1) : domain.top(1);
    // The solver calls this hook only after reaching a normal Return.
}
} // namespace fdulang
