#include "fdulang/lab3.hpp"
#include "analysis_support.hpp"
#include <algorithm>
#include <cassert>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

namespace fdulang {
namespace {

// Mark functions in cyclic SCCs of the static call graph. Iterative Kosaraju
// avoids using the host stack, including for large source-level call chains.
std::vector<bool> recursive_functions(const ProgramCfg& program) {
    const auto count = program.functions.size();
    std::vector<std::vector<std::size_t>> edges(count), reverse(count);
    for (std::size_t f = 0; f < count; ++f)
        for (const auto& node : program.functions[f].nodes)
            if (node.kind == NodeKind::Call) {
                const auto callee = static_cast<std::size_t>(node.callee);
                edges[f].push_back(callee);
                reverse[callee].push_back(f);
            }
    struct Frame { std::size_t function; std::size_t next = 0; };
    std::vector<bool> visited(count, false), recursive(count, false);
    std::vector<std::size_t> order;
    for (std::size_t root = 0; root < count; ++root) {
        if (visited[root]) continue;
        visited[root] = true;
        std::vector<Frame> stack{{root, 0}};
        while (!stack.empty()) {
            auto& frame = stack.back();
            if (frame.next == edges[frame.function].size()) {
                order.push_back(frame.function); stack.pop_back();
            } else {
                const auto child = edges[frame.function][frame.next++];
                if (!visited[child]) { visited[child] = true; stack.push_back({child, 0}); }
            }
        }
    }
    std::fill(visited.begin(), visited.end(), false);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (visited[*it]) continue;
        std::vector<std::size_t> component, pending{*it};
        visited[*it] = true;
        while (!pending.empty()) {
            const auto current = pending.back(); pending.pop_back();
            component.push_back(current);
            for (const auto next : reverse[current])
                if (!visited[next]) { visited[next] = true; pending.push_back(next); }
        }
        const auto root = component.front();
        const bool cyclic = component.size() > 1 ||
            std::find(edges[root].begin(), edges[root].end(), root) != edges[root].end();
        for (const auto f : component) recursive[f] = cyclic;
    }
    return recursive;
}

using Job = std::pair<std::size_t, int>; // Context ID, CFG node ID.
using ContextKey = std::pair<int, CallString>;
struct ContextState {
    std::size_t id;
    int function;
    CallString calls;
    std::vector<std::unique_ptr<AbstractState>> states;
    std::vector<bool> queued;
    std::vector<std::uint64_t> growth;
    std::unique_ptr<AbstractState> returns;
    std::uint64_t return_growth = 0;
    // Each dependent is a call node in a complete caller context.
    // Never reconstruct these from a truncated call string. Several complete
    // callers may share the same callee context, including recursive callers.
    std::set<Job> dependents;
};

enum class MergeKind { Local, Entry, Return };

class InterSolver {
public:
    InterSolver(const ProgramCfg& program, const DomainFactory& domain,
                const AnalysisOptions& options)
        : program_(program), domain_(domain), options_(options),
          recursive_(recursive_functions(program)) {
        assert(domain_.top && domain_.bottom);
        for (std::size_t f = 0; f < program_.functions.size(); ++f) {
            points_.push_back(analysis_detail::widening_points(program_.functions[f]));
            if (recursive_[f]) points_.back()[program_.functions[f].entry] = true;
        }
    }

    AnalysisResult run() {
        auto* main = get_context(program_.entry_function, {});
        if (!main) return result_;
        const auto& cfg = program_.functions[main->function];
        main->states[cfg.entry] = domain_.top(cfg.variables.size());
        enqueue(*main, cfg.entry);

        while (!queue_.empty() && result_.status == AnalysisStatus::Success) {
            if (options_.max_iterations != 0 && result_.stats.iterations >= options_.max_iterations) {
                fail("analysis_iterations", "analysis worklist limit reached before convergence");
                break;
            }
            const auto job = queue_.front();
            queue_.pop_front();
            process_node(job);
        }
        return result_;
    }

private:
    const ProgramCfg& program_;
    const DomainFactory& domain_;
    const AnalysisOptions& options_;
    AnalysisResult result_;
    std::vector<bool> recursive_;
    std::vector<std::vector<bool>> points_;
    std::map<ContextKey, std::size_t> lookup_;
    // Heap-owned contexts remain stable while recursive calls grow this vector.
    std::vector<std::unique_ptr<ContextState>> contexts_;
    std::deque<Job> queue_;
    std::set<int> visited_functions_;

    void fail(const std::string& code, const std::string& message) {
        result_.status = AnalysisStatus::ResourceLimit;
        result_.code = code;
        result_.message = message;
        result_.may_abort.clear();
    }

    void enqueue(ContextState& context, int node) {
        if (!context.queued[node]) {
            context.queued[node] = true;
            queue_.emplace_back(context.id, node);
        }
    }

    ContextState* get_context(int function, const CallString& calls) {
        const ContextKey key{function, calls};
        const auto found = lookup_.find(key);
        if (found != lookup_.end()) return contexts_[found->second].get();
        if (options_.max_contexts != 0 && contexts_.size() >= options_.max_contexts) {
            fail("analysis_contexts", "analysis context limit reached before convergence");
            return nullptr;
        }

        const auto& cfg = program_.functions[function];
        auto state = std::make_unique<ContextState>();
        state->id = contexts_.size();
        state->function = function;
        state->calls = calls;
        for (std::size_t i = 0; i < cfg.nodes.size(); ++i)
            state->states.push_back(domain_.bottom(cfg.variables.size()));
        state->queued.resize(cfg.nodes.size(), false);
        state->growth.resize(cfg.nodes.size(), 0);
        state->returns = domain_.bottom(1);
        lookup_.emplace(key, state->id);
        contexts_.push_back(std::move(state));

        visited_functions_.insert(function);
        result_.analyzed_functions.assign(visited_functions_.begin(), visited_functions_.end());
        result_.stats.functions = visited_functions_.size();
        result_.stats.contexts = contexts_.size();
        for (const bool point : points_[function]) if (point) ++result_.stats.widening_points;
        if (recursive_[function]) ++result_.stats.widening_points; // The return summary too.
        return contexts_.back().get();
    }

    // One widening policy for local loop nodes, recursive entries and recursive
    // return summaries. Always widen a clone of the OLD invariant, preserving
    // the octagon domain's separate raw widening history.
    bool merge(std::unique_ptr<AbstractState>& current, const AbstractState& incoming,
               bool widening, std::uint64_t& growth, MergeKind kind,
               std::size_t dimensions) {
        if (incoming.is_bottom() || incoming.leq(*current)) return false;
        std::unique_ptr<AbstractState> candidate;
        if (kind == MergeKind::Entry)
            candidate = join_call_entry(*current, incoming, domain_, dimensions);
        else if (kind == MergeKind::Return)
            candidate = join_return_summary(*current, incoming, domain_);
        else {
            candidate = current->clone();
            candidate->join_with(incoming);
        }
        ++result_.stats.joins;
        if (!candidate || !current->leq(*candidate) || !incoming.leq(*candidate))
            throw std::logic_error("analysis join does not contain both inputs");
        if (widening && !current->is_bottom()) {
            if (growth >= options_.widen_delay) {
                auto widened = current->clone();
                widened->widen_with(*candidate);
                ++result_.stats.widenings;
                if (!candidate->leq(*widened) || !current->leq(*widened))
                    throw std::logic_error("domain widening is not an upper bound");
                candidate = std::move(widened);
            } else ++growth;
        }
        current = std::move(candidate);
        return true;
    }

    void propagate(ContextState& context, int target, const AbstractState& incoming,
                   MergeKind kind = MergeKind::Local) {
        const int function = context.function;
        if (merge(context.states[target], incoming, points_[function][target],
                  context.growth[target], kind, program_.functions[function].variables.size()))
            enqueue(context, target);
    }

    void process_call(ContextState& caller, const Node& node, AbstractState& output) {
        const auto calls = next_context(caller.calls, node.call_id, options_.context_depth);
        if (calls.size() > options_.context_depth)
            throw std::logic_error("call string exceeds configured depth");
        auto* callee = get_context(node.callee, calls);
        if (!callee) return;
        const auto& cfg = program_.functions[node.callee];

        // Register before propagating, including when a summary already exists.
        callee->dependents.emplace(caller.id, node.id);
        auto bound = bind_call_arguments(output, node, cfg, domain_);
        if (!bound) throw std::logic_error("argument binding returned no state");
        propagate(*callee, cfg.entry, *bound, MergeKind::Entry);
        if (!callee->returns->is_bottom()) {
            output.assign_return(node.target, *callee->returns);
            propagate(caller, node.next, output);
        }
    }

    void process_return(ContextState& context, const Node& node, const AbstractState& output) {
        auto returned = output.remap({node.left});
        if (!merge(context.returns, *returned, recursive_[context.function],
                   context.return_growth, MergeKind::Return, 1)) return;
        ++result_.stats.summary_updates;
        for (const auto& dependent : context.dependents) {
            ++result_.stats.dependency_notifications;
            enqueue(*contexts_[dependent.first], dependent.second);
        }
    }

    void process_node(const Job& job) {
        auto& context = *contexts_[job.first];
        const int current = job.second;
        context.queued[current] = false;
        ++result_.stats.iterations;
        if (context.states[current]->is_bottom()) return;
        const auto& node = program_.functions[context.function].nodes[current];
        auto output = context.states[current]->clone();
        switch (node.kind) {
        case NodeKind::Copy: case NodeKind::Add: case NodeKind::Sub: case NodeKind::Neg:
            output->assign(node.target, node.kind, node.left, node.right);
            propagate(context, node.next, *output);
            break;
        case NodeKind::Input:
            output->forget(node.target);
            propagate(context, node.next, *output);
            break;
        case NodeKind::Branch: {
            auto alternative = output->clone();
            output->assume(node, true);
            alternative->assume(node, false);
            propagate(context, node.true_target, *output);
            propagate(context, node.false_target, *alternative);
            break;
        }
        case NodeKind::Call:
            process_call(context, node, *output);
            break;
        case NodeKind::Return:
            process_return(context, node, *output);
            break;
        case NodeKind::Abort: result_.may_abort.insert(node.abort_id); break;
        case NodeKind::Halt: break;
        case NodeKind::Entry: case NodeKind::Nop:
            for (const int variable : node.forget) output->forget(variable);
            propagate(context, node.next, *output);
            break;
        }
    }
};

} // namespace

AnalysisResult analyze_inter(const ProgramCfg& program, const DomainFactory& domain,
                             const AnalysisOptions& options) {
    return InterSolver(program, domain, options).run();
}
} // namespace fdulang
