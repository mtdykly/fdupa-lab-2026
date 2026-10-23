#ifndef FDULANG_LAB3_HPP
#define FDULANG_LAB3_HPP
#include "fdulang/analysis.hpp"

namespace fdulang {
using CallString = std::vector<int>; // Oldest -> newest static call-site IDs.

// Lab3.1: append this site and retain the final k sites (k=0 means empty).
CallString next_context(const CallString& caller, int call_site, std::size_t k);

// Lab3.2: evaluate all arguments in the OLD caller state simultaneously, then
// bind callee parameters in a fresh namespace. Preserve representable relations.
std::unique_ptr<AbstractState> bind_call_arguments(
    const AbstractState& caller, const Node& call, const FunctionCfg& callee,
    const DomainFactory& domain);

// Lab3.3/3.4: return an upper bound of both inputs. Never replace current with
// incoming: different dynamic calls/returns can share one abstract context.
// Widening, dependency registration and notifications belong to the framework.
std::unique_ptr<AbstractState> join_call_entry(
    const AbstractState& current, const AbstractState& incoming,
    const DomainFactory& domain, std::size_t dimensions);
std::unique_ptr<AbstractState> join_return_summary(
    const AbstractState& current, const AbstractState& incoming,
    const DomainFactory& domain);
} // namespace fdulang
#endif
