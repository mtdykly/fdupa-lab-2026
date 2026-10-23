#include "fdulang/analysis.hpp"
#include "fdulang/interval.hpp"
#include "fdulang/octagon.hpp"
#include "analysis_support.hpp"

#include <deque>
#include <cassert>
#include <stdexcept>
#include <utility>

namespace fdulang {
namespace analysis_detail {

std::vector<int> successors(const Node& node) {
    switch (node.kind) {
    case NodeKind::Branch: return {node.true_target, node.false_target};
    case NodeKind::Return: case NodeKind::Halt: case NodeKind::Abort: return {};
    default: return {node.next};
    }
}

std::vector<int> selected_functions(const ProgramCfg& program) {
    std::vector<bool> selected(program.functions.size(), false);
    std::deque<int> queue;
    selected.at(static_cast<std::size_t>(program.entry_function)) = true;
    queue.push_back(program.entry_function);
    while (!queue.empty()) {
        const int index = queue.front(); queue.pop_front();
        for (const auto& node : program.functions.at(static_cast<std::size_t>(index)).nodes) {
            if (node.kind != NodeKind::Call) continue;
            if (!selected.at(static_cast<std::size_t>(node.callee))) {
                selected[static_cast<std::size_t>(node.callee)] = true;
                queue.push_back(node.callee);
            }
        }
    }
    std::vector<int> result;
    for (std::size_t i = 0; i < selected.size(); ++i)
        if (selected[i]) result.push_back(static_cast<int>(i));
    return result;
}

// Every directed cycle contains a DFS back edge. Widen at its target, so every
// reachable cycle crosses a widening point. The iterative DFS avoids host stack
// overflow; successor order and node numbering completely determine the result.
std::vector<bool> widening_points(const FunctionCfg& function) {
    struct Frame { int node; std::vector<int> edges; std::size_t cursor = 0; };
    std::vector<unsigned char> color(function.nodes.size(), 0);
    std::vector<bool> points(function.nodes.size(), false);
    std::vector<Frame> stack;
    color.at(static_cast<std::size_t>(function.entry)) = 1;
    stack.push_back({function.entry, successors(function.nodes.at(
        static_cast<std::size_t>(function.entry))), 0});
    while (!stack.empty()) {
        Frame& frame = stack.back();
        if (frame.cursor == frame.edges.size()) {
            color[static_cast<std::size_t>(frame.node)] = 2;
            stack.pop_back();
            continue;
        }
        const int target = frame.edges[frame.cursor++];
        const std::size_t index = static_cast<std::size_t>(target);
        if (color.at(index) == 1) points[index] = true;
        else if (color[index] == 0) {
            color[index] = 1;
            stack.push_back({target, successors(function.nodes[index]), 0});
        }
    }
    return points;
}

} // namespace

DomainFactory interval_domain() {
    return {"interval", make_interval_top, make_interval_bottom};
}

DomainFactory octagon_domain() {
    return {"octagon", make_octagon_top, make_octagon_bottom};
}

AnalysisResult analyze_intra(const ProgramCfg& program, const DomainFactory& domain,
                             const AnalysisOptions& options) {
    assert(domain.top && domain.bottom);
    AnalysisResult result;
    result.analyzed_functions = analysis_detail::selected_functions(program);
    result.stats.functions = result.analyzed_functions.size();
    for (const int function_index : result.analyzed_functions) {
        const auto& function = program.functions.at(static_cast<std::size_t>(function_index));
        const auto points = analysis_detail::widening_points(function);
        for (const bool point : points) if (point) ++result.stats.widening_points;
        std::vector<std::unique_ptr<AbstractState>> states;
        for (std::size_t i = 0; i < function.nodes.size(); ++i)
            states.push_back(domain.bottom(function.variables.size()));
        std::vector<bool> queued(function.nodes.size(), false);
        std::vector<std::uint64_t> growth(function.nodes.size(), 0);
        std::deque<int> queue;
        const auto entry = static_cast<std::size_t>(function.entry);
        states[entry] = domain.top(function.variables.size());
        queue.push_back(function.entry); queued[entry] = true;

        auto propagate = [&](int target, const AbstractState& incoming) {
            const auto index = static_cast<std::size_t>(target);
            if (incoming.is_bottom() || incoming.leq(*states.at(index))) return;
            auto candidate = states[index]->clone();
            candidate->join_with(incoming);
            ++result.stats.joins;
            if (!states[index]->leq(*candidate) || !incoming.leq(*candidate))
                throw std::logic_error("domain join does not contain both inputs");
            if (points[index] && !states[index]->is_bottom()) {
                // Initial bottom -> non-bottom does not consume the delay.
                if (growth[index] >= options.widen_delay) {
                    auto widened = states[index]->clone();
                    widened->widen_with(*candidate);
                    ++result.stats.widenings;
                    if (!candidate->leq(*widened) || !states[index]->leq(*widened))
                        throw std::logic_error("domain widening is not an upper bound");
                    candidate = std::move(widened);
                } else ++growth[index];
            }
            states[index] = std::move(candidate);
            if (!queued[index]) { queue.push_back(target); queued[index] = true; }
        };

        while (!queue.empty()) {
            if (options.max_iterations != 0 && result.stats.iterations >= options.max_iterations) {
                result.status = AnalysisStatus::ResourceLimit;
                result.code = "analysis_iterations";
                result.message = "analysis worklist limit reached before convergence";
                result.may_abort.clear(); // An unfinished result is never a proof of safety.
                return result;
            }
            const int current = queue.front(); queue.pop_front();
            const auto index = static_cast<std::size_t>(current);
            queued[index] = false;
            ++result.stats.iterations;
            if (states[index]->is_bottom()) continue;
            const auto& node = function.nodes[index];
            auto output = states[index]->clone();
            switch (node.kind) {
            case NodeKind::Copy: case NodeKind::Add: case NodeKind::Sub: case NodeKind::Neg:
                output->assign(node.target, node.kind, node.left, node.right);
                propagate(node.next, *output);
                break;
            case NodeKind::Input: case NodeKind::Call:
                output->forget(node.target);
                propagate(node.next, *output);
                break;
            case NodeKind::Branch: {
                auto alternative = output->clone();
                output->assume(node, true);
                alternative->assume(node, false);
                propagate(node.true_target, *output);
                propagate(node.false_target, *alternative);
                break;
            }
            case NodeKind::Abort: result.may_abort.insert(node.abort_id); break;
            case NodeKind::Halt: case NodeKind::Return: break;
            case NodeKind::Entry: case NodeKind::Nop:
                for (const int variable : node.forget) output->forget(variable);
                propagate(node.next, *output);
                break;
            }
        }
    }
    return result;
}

} // namespace fdulang
