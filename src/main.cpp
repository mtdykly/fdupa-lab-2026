#include "fdulang/frontend.hpp"
#include "fdulang/ir.hpp"
#include "fdulang/runtime.hpp"
#include "fdulang/analysis.hpp"
#include "fdulang/octagon.hpp"

#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef FDULANG_INTERVAL_IMPLEMENTATION
#define FDULANG_INTERVAL_IMPLEMENTATION "student"
#endif
#ifndef FDULANG_OCTAGON_IMPLEMENTATION
#define FDULANG_OCTAGON_IMPLEMENTATION "student"
#endif
#ifndef FDULANG_INTER_IMPLEMENTATION
#define FDULANG_INTER_IMPLEMENTATION "student"
#endif

namespace {

std::string json_string(const std::string& value) {
    const char* hex = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (c < 32) {
                result += "\\u00";
                result += hex[c >> 4]; result += hex[c & 15];
            } else result += static_cast<char>(c);
        }
    }
    result += '"';
    return result;
}

void location(std::ostream& out, const fdulang::SourceSpan& span) {
    out << "{\"offset\":" << span.begin.offset << ",\"line\":" << span.begin.line
        << ",\"column\":" << span.begin.column << '}';
}

void inventory(std::ostream& out, const fdulang::ProgramCfg& cfg) {
    out << ",\"abort_sites\":[";
    bool first = true;
    for (const auto& site : cfg.abort_sites) {
        if (!first) out << ',';
        first = false;
        out << "{\"id\":\"A" << site.id << "\",\"function\":"
            << json_string(cfg.functions.at(static_cast<std::size_t>(site.function)).name)
            << ",\"location\":";
        location(out, site.span);
        out << '}';
    }
    out << ']';
}

void usage(std::ostream& out) {
    out << "FDULang 0.1\n"
        << "Usage:\n"
        << "  fdulang check FILE [--json]\n"
        << "  fdulang dump-cfg FILE\n"
        << "  fdulang run FILE [--input FILE] [--json] [--max-steps N] [--max-depth N]\n"
        << "  fdulang analyze FILE [--domain interval|octagon] [--mode intra|inter] [--json]\n"
        << "      [--widen-delay N] [--max-iterations N]\n"
        << "      [--k N] [--max-contexts N]  (inter only; defaults: k=1, max-contexts=10000)\n"
        << "Input defaults to stdin. Zero disables the corresponding resource limit.\n";
}

std::uint64_t parse_limit(const std::string& text) {
    if (text.empty()) throw std::invalid_argument("empty limit");
    std::uint64_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9' || value >
            (std::numeric_limits<std::uint64_t>::max() - static_cast<unsigned>(c - '0')) / 10)
            throw std::invalid_argument("limit must be an unsigned decimal integer");
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}

struct Options {
    std::string command, source, source_display, input;
    std::string domain = "interval";
    std::string mode = "intra";
    bool json = false;
    bool execution_option = false;
    bool analysis_option = false;
    bool inter_option = false;
    fdulang::ExecutionOptions limits;
    fdulang::AnalysisOptions analysis;
};

Options parse_options(int argc, char** argv) {
    if (argc < 3) throw std::invalid_argument("a command and source file are required");
    Options options;
    options.command = argv[1];
    if (options.command != "check" && options.command != "dump-cfg" && options.command != "run"
        && options.command != "analyze")
        throw std::invalid_argument("unknown command: " + options.command);
    bool end_options = false;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!end_options && arg == "--") { end_options = true; continue; }
        if (!end_options && arg == "--json") { options.json = true; continue; }
        if (!end_options && (arg == "--domain" || arg == "--mode" ||
                             arg == "--widen-delay" || arg == "--max-iterations" ||
                             arg == "--k" || arg == "--max-contexts")) {
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            options.analysis_option = true;
            const std::string value = argv[i];
            if (arg == "--domain") {
                if (value != "interval" && value != "octagon")
                    throw std::invalid_argument("domain must be interval or octagon");
                options.domain = value;
            } else if (arg == "--mode") {
                if (value != "intra" && value != "inter")
                    throw std::invalid_argument("mode must be intra or inter");
                options.mode = value;
            } else if (arg == "--k" || arg == "--max-contexts") {
                options.inter_option = true;
                const auto limit = parse_limit(value);
                if (limit > std::numeric_limits<std::size_t>::max())
                    throw std::invalid_argument(arg + " is too large for this platform");
                if (arg == "--k") options.analysis.context_depth = static_cast<std::size_t>(limit);
                else options.analysis.max_contexts = static_cast<std::size_t>(limit);
            } else if (arg == "--widen-delay") options.analysis.widen_delay = parse_limit(value);
            else options.analysis.max_iterations = parse_limit(value);
            continue;
        }
        if (!end_options && (arg == "--input" || arg == "--max-steps" || arg == "--max-depth")) {
            if (++i == argc) throw std::invalid_argument("missing value for " + arg);
            options.execution_option = true;
            if (arg == "--input") options.input = argv[i];
            else {
                const auto limit = parse_limit(argv[i]);
                if (arg == "--max-steps") options.limits.max_steps = limit;
                else {
                    if (limit > std::numeric_limits<std::size_t>::max())
                        throw std::invalid_argument("call depth limit is too large for this platform");
                    options.limits.max_call_depth = static_cast<std::size_t>(limit);
                }
            }
            continue;
        }
        if (!end_options && !arg.empty() && arg[0] == '-')
            throw std::invalid_argument("unknown option: " + arg);
        if (!options.source.empty()) throw std::invalid_argument("only one source file is supported");
        options.source = arg;
    }
    if (options.source.empty()) throw std::invalid_argument("a source file is required");
    if (options.command != "run" && options.execution_option)
        throw std::invalid_argument("input and execution limits only apply to run");
    if (options.command != "analyze" && options.analysis_option)
        throw std::invalid_argument("analysis options only apply to analyze");
    if (options.inter_option && options.mode != "inter")
        throw std::invalid_argument("--k and --max-contexts only apply to inter analysis");
    if (options.command == "dump-cfg" && options.json)
        throw std::invalid_argument("dump-cfg currently supports text output only");
    // argv/file opening use the native filename encoding. JSON and diagnostics
    // use UTF-8, including on Windows systems whose native code page is not UTF-8.
    options.source_display = std::filesystem::path(options.source).u8string();
    return options;
}

void error_output(const Options& options, const std::string& status, const std::string& code,
                  const std::string& message, const fdulang::SourceSpan* span) {
    if (options.json) {
        std::cout << "{\"format_version\":1,\"status\":" << json_string(status)
                  << ",\"source\":" << json_string(options.source_display)
                  << ",\"code\":" << json_string(code) << ",\"message\":" << json_string(message);
        if (span) { std::cout << ",\"location\":"; location(std::cout, *span); }
        std::cout << "}\n";
    }
    std::cerr << options.source_display;
    if (span) std::cerr << ':' << span->begin.line << ':' << span->begin.column;
    std::cerr << ": " << code << ": " << message << '\n';
}

void print_analysis_result(const Options& options, const fdulang::ProgramCfg& cfg,
                           const fdulang::AnalysisResult& result) {
    const char* implementation = options.domain == "interval"
        ? FDULANG_INTERVAL_IMPLEMENTATION : FDULANG_OCTAGON_IMPLEMENTATION;
    if (options.mode == "inter" && std::string(FDULANG_INTER_IMPLEMENTATION) == "starter")
        implementation = "starter";
    const bool success = result.status == fdulang::AnalysisStatus::Success;
    if (options.json) {
        std::cout << "{\"format_version\":1,\"status\":"
                  << json_string(success ? "success" : "resource_limit")
                  << ",\"source\":" << json_string(options.source_display)
                  << ",\"code\":" << json_string(result.code)
                  << ",\"config\":{\"domain\":" << json_string(options.domain)
                  << ",\"mode\":" << json_string(options.mode)
                  << ",\"solver_version\":" << json_string(options.mode + "-v1") << ",\"implementation\":"
                  << json_string(implementation)
                  << ",\"widen_delay\":" << options.analysis.widen_delay
                  << ",\"max_iterations\":" << options.analysis.max_iterations;
        if (options.mode == "inter")
            std::cout << ",\"k\":" << options.analysis.context_depth
                      << ",\"max_contexts\":" << options.analysis.max_contexts;
        if (options.domain == "octagon")
            std::cout << ",\"domain_version\":\"octagon-v1\",\"max_variables\":"
                      << fdulang::OctagonState::max_variables;
        std::cout << '}';
        inventory(std::cout, cfg);
        if (success) {
            std::cout << ",\"may_abort\":[";
            bool first = true;
            for (const int id : result.may_abort) {
                if (!first) std::cout << ',';
                first = false; std::cout << "\"A" << id << '"';
            }
            std::cout << ']';
        }
        std::cout << ",\"stats\":{\"iterations\":" << result.stats.iterations
                  << ",\"joins\":" << result.stats.joins
                  << ",\"widenings\":" << result.stats.widenings
                  << ",\"functions\":" << result.stats.functions
                  << ",\"widening_points\":" << result.stats.widening_points;
        if (options.mode == "inter")
            std::cout << ",\"contexts\":" << result.stats.contexts
                      << ",\"summary_updates\":" << result.stats.summary_updates
                      << ",\"dependency_notifications\":" << result.stats.dependency_notifications;
        std::cout << '}';
        if (!result.message.empty()) std::cout << ",\"message\":" << json_string(result.message);
        std::cout << "}\n";
    } else if (success) {
        std::cout << "analysis success (" << options.domain << '/' << options.mode << ", " << implementation << "): "
                  << result.may_abort.size() << " possible abort site(s)\n";
        for (const auto& site : cfg.abort_sites)
            if (result.may_abort.count(site.id))
                std::cout << "A" << site.id << " at " << options.source_display << ':'
                          << site.span.begin.line << ':' << site.span.begin.column << '\n';
        std::cout << result.stats.iterations << " worklist iterations, "
                  << result.stats.widenings << " widening application(s)\n";
    }
    if (!success) std::cerr << options.source_display << ": " << result.code << ": "
                            << result.message << '\n';
}

int analyze(const Options& options, const fdulang::ProgramCfg& cfg) {
    const auto domain = options.domain == "interval" ? fdulang::interval_domain() : fdulang::octagon_domain();
    fdulang::AnalysisResult result;
    try {
        result = options.mode == "inter" ? fdulang::analyze_inter(cfg, domain, options.analysis)
                                         : fdulang::analyze_intra(cfg, domain, options.analysis);
    } catch (const fdulang::OctagonResourceLimit& error) {
        result.status = fdulang::AnalysisStatus::ResourceLimit;
        result.code = "octagon_variables";
        result.message = error.what();
    }
    print_analysis_result(options, cfg, result);
    return result.status == fdulang::AnalysisStatus::Success ? 0 : 3;
}

void print_check_result(const Options& options, const fdulang::ProgramCfg& cfg) {
    if (options.json) {
        std::cout << "{\"format_version\":1,\"status\":\"checked\",\"source\":"
                  << json_string(options.source_display) << ",\"functions\":" << cfg.functions.size();
        inventory(std::cout, cfg); std::cout << "}\n";
    } else std::cout << "checked: " << cfg.functions.size() << " function(s), "
                     << cfg.abort_sites.size() << " static abort site(s)\n";
}

void print_execution_result(const Options& options, const fdulang::ProgramCfg& cfg,
                            const fdulang::ExecutionResult& result) {
    const std::string status = fdulang::status_name(result.status);
    if (options.json) {
        std::cout << "{\"format_version\":1,\"status\":" << json_string(status)
                  << ",\"source\":" << json_string(options.source_display)
                  << ",\"code\":" << json_string(result.code)
                  << ",\"steps\":" << result.steps << ",\"location\":";
        location(std::cout, result.span);
        if (result.abort_id != 0) std::cout << ",\"abort_id\":\"A" << result.abort_id << '"';
        if (!result.message.empty()) std::cout << ",\"message\":" << json_string(result.message);
        inventory(std::cout, cfg); std::cout << "}\n";
    } else {
        std::cout << status;
        if (result.abort_id != 0) std::cout << " A" << result.abort_id;
        std::cout << " at " << options.source_display << ':' << result.span.begin.line << ':'
                  << result.span.begin.column << " (" << result.steps << " steps)\n";
    }
    if (!result.message.empty()) std::cerr << options.source_display << ':' << result.span.begin.line
        << ':' << result.span.begin.column << ": " << result.code << ": " << result.message << '\n';
}

int run(const Options& options, const fdulang::ProgramCfg& cfg) {
    std::ifstream input_file;
    std::istream* input = &std::cin;
    if (!options.input.empty() && options.input != "-") {
        input_file.open(options.input, std::ios::binary);
        if (!input_file) {
            error_output(options, "io_error", "input_open", "cannot open input file", nullptr);
            return 66;
        }
        input = &input_file;
    }
    const auto result = fdulang::execute(cfg, *input, options.limits);
    print_execution_result(options, cfg, result);
    switch (result.status) {
    case fdulang::ExecutionStatus::Normal: return 0;
    case fdulang::ExecutionStatus::Abort: return 1;
    case fdulang::ExecutionStatus::RuntimeError: return 2;
    case fdulang::ExecutionStatus::ResourceLimit: return 3;
    }
    return 70;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") { usage(std::cout); return 0; }
    Options options;
    try { options = parse_options(argc, argv); }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; usage(std::cerr); return 64;
    }
    try {
        std::ifstream source_file(options.source, std::ios::binary);
        if (!source_file) {
            error_output(options, "io_error", "source_open", "cannot open source file", nullptr);
            return 66;
        }
        const std::string source((std::istreambuf_iterator<char>(source_file)),
                                 std::istreambuf_iterator<char>());
        if (source_file.bad()) {
            error_output(options, "io_error", "source_read", "cannot read source file", nullptr);
            return 66;
        }
        auto ast = fdulang::parse_and_check(options.source_display, source);
        auto cfg = fdulang::lower_to_cfg(ast);
        if (options.command == "dump-cfg") { fdulang::dump_cfg(cfg, std::cout); return 0; }
        if (options.command == "analyze") return analyze(options, cfg);
        if (options.command == "check") { print_check_result(options, cfg); return 0; }
        return run(options, cfg);
    } catch (const fdulang::Diagnostic& error) {
        error_output(options, "frontend_error", error.category, error.what(), &error.span);
        return 65;
    } catch (const std::exception& error) {
        error_output(options, "internal_error", "internal_error", error.what(), nullptr);
        return 70;
    }
}
