#ifndef FDULANG_ANALYSIS_HPP
#define FDULANG_ANALYSIS_HPP

#include "fdulang/domain.hpp"
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fdulang {

// The solver only knows this interface, never a concrete domain representation.
struct DomainFactory {
    std::string name;
    std::unique_ptr<AbstractState> (*top)(std::size_t);
    std::unique_ptr<AbstractState> (*bottom)(std::size_t);
};
DomainFactory interval_domain();
DomainFactory octagon_domain();

struct AnalysisOptions {
    std::uint64_t widen_delay = 2;
    std::uint64_t max_iterations = 1000000; // Queue removals; zero disables limit.
    std::size_t context_depth = 1;
    std::size_t max_contexts = 10000; // Zero disables limit.
};
struct AnalysisStats {
    std::uint64_t iterations = 0;
    std::uint64_t joins = 0;
    std::uint64_t widenings = 0;
    std::size_t functions = 0;
    std::size_t widening_points = 0;
    std::size_t contexts = 0;
    std::uint64_t summary_updates = 0;
    std::uint64_t dependency_notifications = 0;
};
enum class AnalysisStatus { Success, ResourceLimit };
struct AnalysisResult {
    AnalysisStatus status = AnalysisStatus::Success;
    std::string code = "ok";
    std::string message;
    std::set<int> may_abort;
    std::vector<int> analyzed_functions;
    AnalysisStats stats;
};

// Calls are discovered syntactically, including in unreachable CFG fragments.
// Every selected function starts independently at top. Calls may return top.
AnalysisResult analyze_intra(const ProgramCfg& program, const DomainFactory& domain,
                             const AnalysisOptions& options = {});

// Start only main's empty context. Calls join into (callee, last k call sites),
// while normal returns notify the complete caller contexts that depend on them.
AnalysisResult analyze_inter(const ProgramCfg& program, const DomainFactory& domain,
                             const AnalysisOptions& options = {});

} // namespace fdulang
#endif
