#include "fdulang/analysis.hpp"
#include "fdulang/frontend.hpp"
#include "fdulang/interval.hpp"
#include "fdulang/lab3.hpp"
#include "fdulang/octagon.hpp"

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace fdulang;

struct Test {
    std::string name;
    std::function<void()> run;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ProgramCfg compile(const std::string& source) {
    return lower_to_cfg(parse_and_check("interprocedural-unit.fdulang", source));
}

AnalysisResult analyze(const std::string& source, const DomainFactory& domain,
                       std::size_t depth = 1, const AnalysisOptions& settings = {}) {
    auto options = settings;
    options.context_depth = depth;
    return analyze_inter(compile(source), domain, options);
}

void success(const AnalysisResult& result) {
    require(result.status == AnalysisStatus::Success,
            "analysis did not finish: " + result.code + " " + result.message);
}

void expect(const std::string& source, const DomainFactory& domain,
            const std::set<int>& aborts, std::size_t depth = 1) {
    const auto result = analyze(source, domain, depth);
    success(result);
    require(result.may_abort == aborts, "unexpected abort set");
}

void equal(const AbstractState& actual, const AbstractState& expected) {
    require(actual.leq(expected) && expected.leq(actual), "unexpected abstract state");
}

void interval_is(const Interval& value, std::int64_t lower, std::int64_t upper) {
    require(!value.empty() && value.lower == lower && value.upper == upper,
            "unexpected interval");
}

std::vector<Test> tests() {
    std::vector<Test> result;
    auto add = [&](const std::string& name, std::function<void()> run) {
        result.push_back({name, std::move(run)});
    };

    add("hooks/context_k0", [] {
        require(next_context({}, 30, 0).empty(), "k=0 must discard the new site");
        require(next_context({10, 20}, 30, 0).empty(), "k=0 must discard all prior sites");
    });
    add("hooks/context_k1", [] {
        const CallString caller{10, 20};
        require(next_context({}, 30, 1) == CallString{30}, "first call site was lost");
        require(next_context(caller, 30, 1) == CallString{30}, "k=1 must retain only the newest site");
        require(caller == CallString({10, 20}), "context extension changed the caller");
    });
    add("hooks/context_k2", [] {
        require(next_context({}, 30, 2) == CallString{30}, "short context must not be padded");
        require(next_context({20}, 30, 2) == CallString({20, 30}), "context order must be oldest to newest");
        require(next_context({10, 20}, 30, 2) == CallString({20, 30}), "k=2 must retain the final two sites");
    });

    const std::vector<DomainFactory> domains = {interval_domain(), octagon_domain()};
    for (const auto& domain : domains) {
        const std::string prefix = domain.name + "/";
        add("hooks/" + prefix + "bind_repeated_and_constant_arguments", [domain] {
            auto caller = domain.top(2);
            caller->assign(1, NodeKind::Copy, Operand::literal(-2), Operand::literal(0));
            const auto original = caller->clone();
            Node call;
            call.kind = NodeKind::Call;
            call.arguments = {Operand::slot(0), Operand::literal(4), Operand::slot(0), Operand::slot(1)};
            FunctionCfg callee;
            callee.variables.resize(5);
            callee.parameters = {3, 1, 4, 0};
            const auto bound = bind_call_arguments(*caller, call, callee, domain);
            require(static_cast<bool>(bound), "argument binding returned no state");
            auto expected = domain.top(5);
            expected->assign(0, NodeKind::Copy, Operand::literal(-2), Operand::literal(0));
            expected->assign(1, NodeKind::Copy, Operand::literal(4), Operand::literal(0));
            // Octagons retain parameter 4 == parameter 3; intervals retain
            // their separate ranges. The unmapped local at slot 2 stays top.
            expected->assign(4, NodeKind::Copy, Operand::slot(3), Operand::literal(0));
            equal(*bound, *expected);
            equal(*caller, *original);
        });
        add("hooks/" + prefix + "entry_merge_contains_both_inputs", [domain] {
            auto current = domain.top(2), incoming = domain.top(2);
            current->assign(0, NodeKind::Copy, Operand::literal(0), Operand::literal(0));
            current->assign(1, NodeKind::Copy, Operand::literal(1), Operand::literal(0));
            incoming->assign(0, NodeKind::Copy, Operand::literal(2), Operand::literal(0));
            incoming->assign(1, NodeKind::Copy, Operand::literal(3), Operand::literal(0));
            const auto old_current = current->clone(), old_incoming = incoming->clone();
            const auto joined = join_call_entry(*current, *incoming, domain, 2);
            require(static_cast<bool>(joined), "entry merge returned no state");
            require(current->leq(*joined), "entry merge lost the earlier call");
            require(incoming->leq(*joined), "entry merge lost the incoming call");
            equal(*current, *old_current);
            equal(*incoming, *old_incoming);
        });
        add("hooks/" + prefix + "summary_merge_contains_both_inputs", [domain] {
            auto current = domain.top(1), incoming = domain.top(1);
            current->assign(0, NodeKind::Copy, Operand::literal(-2), Operand::literal(0));
            incoming->assign(0, NodeKind::Copy, Operand::literal(5), Operand::literal(0));
            const auto old_current = current->clone(), old_incoming = incoming->clone();
            const auto joined = join_return_summary(*current, *incoming, domain);
            require(static_cast<bool>(joined), "summary merge returned no state");
            require(current->leq(*joined), "summary merge lost an earlier return");
            require(incoming->leq(*joined), "summary merge lost the incoming return");
            equal(*current, *old_current);
            equal(*incoming, *old_incoming);
        });
        add(prefix + "main_abort", [domain] {
            expect("fn main(){abort;}", domain, {1});
        });
        add(prefix + "constant_parameter_and_return", [domain] {
            expect("fn f(v){if(v!=3){abort;} return v;}"
                   "fn main(){let r=f(3); if(r!=3){abort;}}", domain, {});
        });
        add(prefix + "computed_return", [domain] {
            expect("fn main(){let x=7; let r=increment(x); if(r!=8){abort;}}"
                   "fn increment(v){return v+1;}", domain, {});
        });
        add(prefix + "implicit_zero_return", [domain] {
            expect("fn f(){let x=9;} fn main(){let r=f(); if(r!=0){abort;}}", domain, {});
        });
        add(prefix + "callee_halt_stops_caller", [domain] {
            expect("fn stop(){halt;} fn main(){stop(); abort;}", domain, {});
        });
        add(prefix + "callee_abort_stops_caller", [domain] {
            expect("fn stop(){abort; return 0;} fn main(){stop(); abort;}", domain, {1});
        });
        add(prefix + "endless_recursion_has_no_return", [domain] {
            for (std::size_t depth = 0; depth <= 2; ++depth) {
                expect("fn f(){return f();} fn main(){f(); abort;}", domain, {}, depth);
            }
        });
        add(prefix + "only_reached_calls_create_callees", [domain] {
            const auto analysis = analyze(
                "fn unused(){abort;} fn dead(){abort;}"
                "fn main(){if(0==1){dead();} halt;}", domain);
            success(analysis);
            require(analysis.may_abort.empty(), "unreached function was analyzed at top");
            require(analysis.stats.functions == 1 && analysis.stats.contexts == 1,
                    "unreached calls created contexts");
        });
        add(prefix + "call_expression_execution_order", [domain] {
            expect("fn left(){halt;} fn right(){abort;}"
                   "fn main(){let r=left()+right(); abort;}", domain, {});
        });
        add(prefix + "callee_local_namespace_and_value_parameters", [domain] {
            expect("fn f(x){x=99; let y=100; return 2;}"
                   "fn main(){let x=7; let y=3; let r=f(x);"
                   "if(x!=7){abort;} if(y!=3){abort;} if(r!=2){abort;}}", domain, {});
        });
        add(prefix + "multiple_return_sites", [domain] {
            expect("fn f(v){if(v==0){return 2;} return 4;}"
                   "fn main(){let x=0; input(x); let r=f(x);"
                   "if(r<2){abort;} if(r>4){abort;}}", domain, {});
        });
        add(prefix + "return_summary_is_convex", [domain] {
            // The reachable results are 2 and 4. Both domains admit their hull.
            expect("fn f(v){if(v==0){return 2;} return 4;}"
                   "fn main(){let x=0; input(x); let r=f(x); if(r==3){abort;}}", domain, {1});
        });
        add(prefix + "distinct_static_call_sites", [domain] {
            const std::string source =
                "fn probe(v){if(v==1){abort;} return 0;}"
                "fn main(){probe(0); probe(2);}";
            expect(source, domain, {1}, 0);
            expect(source, domain, {}, 1);
            expect(source, domain, {}, 2);
        });
        add(prefix + "two_levels_separate_shared_inner_call", [domain] {
            const std::string source =
                "fn probe(v){if(v==1){abort;} return 0;}"
                "fn wrapper(v){return probe(v);}"
                "fn main(){wrapper(0); wrapper(2);}";
            expect(source, domain, {1}, 0);
            expect(source, domain, {1}, 1);
            expect(source, domain, {}, 2);
        });
        add(prefix + "context_precision_keeps_real_abort", [domain] {
            const std::string source =
                "fn probe(v){if(v==1){abort;} return 0;}"
                "fn wrapper(v){return probe(v);}"
                "fn main(){wrapper(0); wrapper(1);}";
            for (std::size_t depth = 0; depth <= 2; ++depth) expect(source, domain, {1}, depth);
        });
        add(prefix + "call_site_ids_are_global", [domain] {
            expect("fn probe(v){if(v==1){abort;} return 0;}"
                   "fn left(){return probe(0);} fn right(){return probe(2);}"
                   "fn main(){left(); right();}", domain, {}, 1);
        });
        add(prefix + "return_restores_full_caller_context", [domain] {
            // Both wrapper contexts call the same id() context for k=1. Its
            // truncated context cannot recover either wrapper continuation.
            const std::string source =
                "fn id(){return 0;}"
                "fn wrapper(v){let r=id(); if(v==2){abort;} return r;}"
                "fn main(){wrapper(0); wrapper(2);}";
            expect(source, domain, {1}, 1);
            expect(source, domain, {1}, 2);
        });
        add(prefix + "existing_summary_reaches_new_caller", [domain] {
            expect("fn f(){return 7;} fn main(){let a=f(); let b=f();"
                   "if(a!=7){abort;} if(b==7){abort;}}", domain, {2}, 0);
        });
        add(prefix + "summary_growth_revisits_earlier_caller", [domain] {
            const std::string source =
                "fn f(v){if(v==0){return 0;} return 1;}"
                "fn main(){let a=f(0); let b=f(1); if(a==1){abort;}}";
            expect(source, domain, {1}, 0);
            expect(source, domain, {}, 1);
        });
        add(prefix + "recursive_base_return_wakes_callers", [domain] {
            const std::string source =
                "fn f(v){if(v<=0){return 7;} return f(v-1);}"
                "fn main(){let r=f(3); if(r!=7){abort;} if(r==7){abort;}}";
            for (std::size_t depth = 0; depth <= 2; ++depth) expect(source, domain, {2}, depth);
        });
        add(prefix + "mutual_recursion", [domain] {
            expect("fn f(v){if(v<=0){return 9;} return g(v-1);}"
                   "fn g(v){if(v<=0){return 9;} return f(v-1);}"
                   "fn main(){let r=f(4); if(r!=9){abort;} if(r==9){abort;}}", domain, {2});
        });
        add(prefix + "recursive_abort_without_normal_return", [domain] {
            expect("fn f(v){if(v==0){abort;} return f(v-1);}"
                   "fn main(){f(3); abort;}", domain, {1});
        });
        add(prefix + "explicit_main_call_returns_to_its_caller", [domain] {
            expect("fn main(){let x=0; input(x); if(x==0){return 7;}"
                   "let r=main(); if(r!=7){abort;} if(r==7){abort;} return 7;}", domain, {2});
        });
        add(prefix + "input_overwrites_only_callee_parameter", [domain] {
            expect("fn f(v){input(v); return v;}"
                   "fn main(){let x=1; let r=f(x); if(x!=1){abort;} if(r==2){abort;}}", domain, {2});
        });
        add(prefix + "overflowing_return_is_not_normal_return", [domain] {
            expect("fn f(v){if(v==0){return 2147483647;} return f(v-1)+1;}"
                   "fn main(){let r=f(1); abort;}", domain, {}, 1);
        });
        add(prefix + "recursive_entry_widening", [domain] {
            AnalysisOptions options;
            options.widen_delay = 0;
            options.max_iterations = 5000;
            const auto analysis = analyze(
                "fn f(v){if(v>=100000){return 0;} return f(v+1);}"
                "fn main(){f(0); abort;}", domain, 1, options);
            success(analysis);
            require(analysis.may_abort == std::set<int>{1}, "lost normal recursive return");
            require(analysis.stats.widenings > 0, "recursive entry did not use widening");
        });
        add(prefix + "recursive_return_summary_widening", [domain] {
            AnalysisOptions options;
            options.widen_delay = 0;
            options.max_iterations = 5000;
            const auto analysis = analyze(
                "fn f(v){if(v==0){return 0;} let r=f(v-1); return r+1;}"
                "fn main(){let x=0; input(x); if(x<0){halt;}"
                "let r=f(x); if(r<0){abort;}}", domain, 1, options);
            success(analysis);
            require(analysis.may_abort.empty(), "nonnegative recursive returns became negative");
            require(analysis.stats.widenings > 0, "recursive summaries did not use widening");
        });
        add(prefix + "iteration_resource_limit_is_failure", [domain] {
            AnalysisOptions options;
            options.max_iterations = 1;
            const auto analysis = analyze("fn f(){return 0;} fn main(){f(); abort;}", domain, 1, options);
            require(analysis.status == AnalysisStatus::ResourceLimit, "iteration limit reported success");
            require(analysis.may_abort.empty(), "partial warnings escaped a failed analysis");
        });
        add(prefix + "context_resource_limit_is_failure", [domain] {
            AnalysisOptions options;
            options.max_contexts = 1;
            const auto analysis = analyze("fn f(){return 0;} fn main(){f(); abort;}", domain, 1, options);
            require(analysis.status == AnalysisStatus::ResourceLimit, "context limit reported success");
            require(analysis.may_abort.empty(), "partial warnings escaped a failed analysis");
        });
        add(prefix + "disabled_limits", [domain] {
            AnalysisOptions options;
            options.max_iterations = 0;
            options.max_contexts = 0;
            const auto analysis = analyze("fn f(){return 0;} fn main(){f(); abort;}", domain, 2, options);
            success(analysis);
            require(analysis.may_abort == std::set<int>{1}, "disabled limits changed result");
        });
        add(prefix + "deterministic_fixed_point", [domain] {
            AnalysisOptions options;
            options.widen_delay = 0;
            const std::string source =
                "fn f(v){if(v>=100){return 2;} return f(v+1);}"
                "fn main(){let r=f(0); if(r==2){abort;}}";
            const auto first = analyze(source, domain, 2, options);
            const auto second = analyze(source, domain, 2, options);
            success(first);
            success(second);
            require(first.may_abort == second.may_abort &&
                    first.analyzed_functions == second.analyzed_functions &&
                    first.stats.iterations == second.stats.iterations &&
                    first.stats.joins == second.stats.joins &&
                    first.stats.widenings == second.stats.widenings &&
                    first.stats.contexts == second.stats.contexts &&
                    first.stats.summary_updates == second.stats.summary_updates &&
                    first.stats.dependency_notifications == second.stats.dependency_notifications,
                    "identical analyses produced different results or counters");
        });
        add(prefix + "scalar_return_boundary", [domain] {
            // The specified summary exports only a scalar interval. It does
            // not relate the returned value to the caller's original x.
            expect("fn identity(v){return v;}"
                   "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;}"
                   "let r=identity(x); if(r!=x){abort;}}", domain, {1}, 2);
        });
    }

    add("bridge/interval_embed_reorders_and_extends", [] {
        IntervalState source(2);
        source.assign(0, NodeKind::Copy, Operand::literal(3), Operand::literal(0));
        source.assign(1, NodeKind::Copy, Operand::literal(4), Operand::literal(0));
        const auto embedded = source.embed(4, {2, 0});
        const auto& state = dynamic_cast<const IntervalState&>(*embedded);
        interval_is(state.value(0), 4, 4);
        interval_is(state.value(1), integer_min, integer_max);
        interval_is(state.value(2), 3, 3);
        interval_is(state.value(3), integer_min, integer_max);
    });
    add("bridge/embed_preserves_bottom", [] {
        const IntervalState interval(2, true);
        const OctagonState octagon(2, true);
        require(interval.embed(4, {2, 0})->is_bottom(), "interval embedding lost bottom");
        require(octagon.embed(4, {2, 0})->is_bottom(), "octagon embedding lost bottom");
    });
    add("bridge/octagon_embed_preserves_relations", [] {
        OctagonState source(2);
        source.constrain(0, 1, 1, 1, 10);
        source.constrain(0, -1, 1, -1, -10);
        const auto embedded = source.embed(4, {3, 1});
        OctagonState expected(4);
        expected.constrain(3, 1, 1, 1, 10);
        expected.constrain(3, -1, 1, -1, -10);
        equal(*embedded, expected);
    });
    add("bridge/interval_return_preserves_other_locals", [] {
        IntervalState caller(2);
        caller.assign(0, NodeKind::Copy, Operand::literal(7), Operand::literal(0));
        caller.assign(1, NodeKind::Copy, Operand::literal(9), Operand::literal(0));
        IntervalState scalar(1);
        scalar.assign(0, NodeKind::Copy, Operand::literal(3), Operand::literal(0));
        caller.assign_return(1, scalar);
        interval_is(caller.value(0), 7, 7);
        interval_is(caller.value(1), 3, 3);
    });
    add("bridge/octagon_return_preserves_unrelated_relations", [] {
        OctagonState caller(3);
        caller.constrain(0, 1, 1, 1, 10);
        caller.constrain(0, -1, 1, -1, -10);
        caller.assign(2, NodeKind::Copy, Operand::literal(4), Operand::literal(0));
        OctagonState scalar(1);
        scalar.assign(0, NodeKind::Copy, Operand::literal(7), Operand::literal(0));
        caller.assign_return(2, scalar);
        OctagonState expected(3);
        expected.constrain(0, 1, 1, 1, 10);
        expected.constrain(0, -1, 1, -1, -10);
        expected.assign(2, NodeKind::Copy, Operand::literal(7), Operand::literal(0));
        equal(caller, expected);
    });
    add("bridge/octagon_return_forgets_target_relations", [] {
        OctagonState caller(2);
        caller.constrain(0, -1, -1, 1, 0);
        caller.constrain(0, 1, -1, 1, 10);
        caller.assign(1, NodeKind::Copy, Operand::slot(0), Operand::literal(0));
        OctagonState scalar(1);
        scalar.constrain(0, -1, -1, 1, 0);
        scalar.constrain(0, 1, -1, 1, 10);
        caller.assign_return(1, scalar);
        OctagonState expected(2);
        for (int variable = 0; variable < 2; ++variable) {
            expected.constrain(variable, -1, -1, 1, 0);
            expected.constrain(variable, 1, -1, 1, 10);
        }
        equal(caller, expected);
    });
    add("bridge/bottom_return_has_no_continuation", [] {
        IntervalState interval(1);
        interval.assign_return(0, IntervalState(1, true));
        OctagonState octagon(1);
        octagon.assign_return(0, OctagonState(1, true));
        require(interval.is_bottom() && octagon.is_bottom(), "bottom return continued caller");
    });

    const std::vector<std::pair<std::string, std::string>> relational_calls = {
        {"duplicate_arguments", "fn check(a,b){if(a!=b){abort;} return 0;}"
             "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;} check(x,x);}"},
        {"sum_arguments", "fn check(a,b){if(a+b!=10){abort;} return 0;}"
             "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=10-x; check(x,y);}"},
        {"reordered_arguments", "fn check(a,b){if(a-b!=3){abort;} return 0;}"
             "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=x+3; check(y,x);}"},
        {"expression_arguments", "fn check(a,b){if(a-b!=1){abort;} return 0;}"
             "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;} check(x+1,x);}"},
        {"caller_relation_survives_call", "fn f(v){return 0;}"
             "fn main(){let x=0; input(x); if(x<0){halt;} if(x>10){halt;} let y=10-x;"
             "let r=f(x); if(x+y!=10){abort;}}"}
    };
    for (const auto& item : relational_calls) {
        add("relations/" + item.first, [item] {
            expect(item.second, interval_domain(), {1});
            expect(item.second, octagon_domain(), {});
        });
    }
    return result;
}
} // namespace

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    int executed = 0, passed = 0;
    for (const auto& test : tests()) {
        if (filter == "--list") { std::cout << test.name << '\n'; continue; }
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        ++executed;
        try { test.run(); ++passed; std::cout << "PASS " << test.name << '\n'; }
        catch (const std::exception& error) { std::cout << "FAIL " << test.name << ": " << error.what() << '\n'; }
    }
    if (filter == "--list") return 0;
    if (executed == 0) { std::cerr << "No matching interprocedural tests\n"; return 2; }
    std::cout << "Interprocedural unit tests: " << executed << ", passed: " << passed
              << ", failed: " << executed - passed << '\n';
    return passed == executed ? 0 : 1;
}
