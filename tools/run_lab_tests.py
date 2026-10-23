#!/usr/bin/env python3
"""Build lab implementations and compare configurations on one shared manifest."""

import argparse
import csv
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace

import evaluate_labs as evaluator


def comparison_report(reports, returncodes):
    """Keep configuration reports separate; never pool runs into one accuracy."""
    if not reports or set(reports) != set(returncodes):
        raise ValueError("every configuration needs a report and exit code")
    hashes = {report.get("manifest_sha256") for report in reports.values()}
    if len(hashes) != 1 or None in hashes:
        raise ValueError("domain reports do not describe the same manifest")
    cases = None
    for label, report in reports.items():
        config = report.get("config", {})
        if configuration_key(config.get("domain"), config.get("mode", "intra"), config.get("k")) != label:
            raise ValueError("report has the wrong configuration")
        current = [(case["id"], case["source_sha256"]) for case in report["cases"]]
        if cases is None:
            cases = current
        elif cases != current:
            raise ValueError("domain reports do not describe the same case inventory")
    return {"format_version": 1, "kind": "fdulang-lab-comparison",
            "manifest_sha256": next(iter(hashes)),
            "complete": all(report["complete"] for report in reports.values()),
            "passed": all(code == 0 for code in returncodes.values()),
            "exit_codes": returncodes,
            ("domains" if all(report.get("config", {}).get("mode", "intra") == "intra"
                              for report in reports.values()) else "configurations"): reports}


def configuration_key(domain, mode, k=None):
    if domain not in ("interval", "octagon") or mode not in ("intra", "inter"):
        raise ValueError("unknown analysis configuration")
    if mode == "intra":
        return domain
    if type(k) is not int or k < 0:
        raise ValueError("inter configuration needs a nonnegative k")
    return "{}-inter-k{}".format(domain, k)


def write_comparison_csv(path, reports):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        inter = any(report.get("config", {}).get("mode") == "inter" for report in reports.values())
        fields = ["domain"] + (["mode", "k", "configuration"] if inter else [])
        fields += ["id", "status", "may_abort", "TP", "FP", "TN", "FN", "reference_match", "error"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for label, report in reports.items():
            for case in report["cases"]:
                row = {key: case.get(key, "") for key in ("id", "status", "reference_match", "error")}
                config = report["config"]
                row.update(domain=config["domain"], may_abort=" ".join(case.get("may_abort", [])))
                if inter:
                    row.update(mode=config.get("mode", "intra"), k=config.get("k", ""), configuration=label)
                row.update(case.get("confusion", {}))
                writer.writerow(row)


def print_comparison(reports):
    print("\nSame-manifest comparison (each row has its own abort denominator):")
    print("configuration       complete  TP  FP  TN  FN  accuracy  FPR       FNR")
    for label, report in reports.items():
        counts, metrics = report["confusion"], report["metrics"]
        def percent(name):
            value = metrics[name]
            return "N/A" if value is None else "{:.2%}".format(value)
        print("{:<19} {:>2}/{:<2}     {:>2}  {:>2}  {:>2}  {:>2}  {:>8}  {:>8}  {:>8}".format(
            label, report["cases_completed"], report["cases_total"],
            counts["TP"], counts["FP"], counts["TN"], counts["FN"],
            percent("accuracy"), percent("fpr"), percent("fnr")))
        if not report["complete"]:
            print("  INCOMPLETE: this row only covers successful cases, not a complete grade.")


def add_analysis_arguments(parser):
    parser.add_argument("--domain", choices=["interval", "octagon", "all"], default="interval")
    parser.add_argument("--mode", choices=["intra", "inter"], default="intra")
    parser.add_argument("--k", help="inter context depth or all (0, 1, 2); default: 1")
    parser.add_argument("--max-contexts", type=int, help="inter only; default: 10000")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--widen-delay", type=int, default=2)
    parser.add_argument("--max-iterations", type=int, default=1000000)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--csv-out", type=Path)


def configurations(args, parser):
    if args.mode == "intra" and (args.k is not None or args.max_contexts is not None):
        parser.error("--k and --max-contexts only apply to --mode inter")
    args.max_contexts = 10000 if args.max_contexts is None else args.max_contexts
    if min(args.widen_delay, args.max_iterations, args.max_contexts) < 0:
        parser.error("analysis limits must be nonnegative")
    if args.k == "all":
        depths = [0, 1, 2]
    elif args.k is None:
        depths = [1] if args.mode == "inter" else [None]
    elif args.k.isascii() and args.k.isdecimal():
        depths = [int(args.k)]
    else:
        parser.error("--k must be a nonnegative decimal integer or all")
    domains = ["interval", "octagon"] if args.domain == "all" else [args.domain]
    result = []
    for domain in domains:
        for depth in depths:
            options = dict(vars(args), domain=domain, k=depth)
            result.append((configuration_key(domain, args.mode, depth),
                           evaluator.config_for(SimpleNamespace(**options))))
    return result


def check_labels(domain, mode):
    labels = ["labs-common"]
    domains = ["interval", "octagon"] if domain == "all" else [domain]
    for selected in domains:
        labels.append("lab1" if selected == "interval" else "lab2")
    if mode == "inter":
        labels.append("lab3-context")
        labels.extend("lab3-" + selected for selected in domains)
        if domain == "all":
            labels.append("lab3-comparison")
    return "^(" + "|".join(labels) + ")$"


def build_binary(root, build, config, jobs, generator=None, cmake_options=(), *, test_labels="^labs$"):
    cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
    if not cmake or not ctest:
        raise ValueError("CMake and CTest must be available on PATH")
    configure = [cmake, "-S", str(root), "-B", str(build),
                 "-DFDULANG_BUILD_LAB_TESTS=ON", "-DFDULANG_BUILD_LANGUAGE_TESTS=OFF",
                 "-DPython3_EXECUTABLE=" + sys.executable, "-DCMAKE_BUILD_TYPE=" + config]
    configure += list(cmake_options)
    if generator:
        configure += ["-G", generator]
    subprocess.run(configure, check=True, cwd=root)
    subprocess.run([cmake, "--build", str(build), "--config", config,
                    "--parallel", str(jobs)], check=True, cwd=root)
    subprocess.run([ctest, "-C", config, "--output-on-failure", "-L", test_labels], check=True, cwd=build)
    name = "fdulang.exe" if sys.platform == "win32" else "fdulang"
    for candidate in (build / config / name, build / name):
        if candidate.is_file():
            return candidate
    raise OSError("built fdulang executable was not found")


def run_evaluations(binary, selected, manifest, expected_paths):
    """Call the evaluator directly; only the C++ analyzer needs a subprocess."""
    reports, codes = {}, {}
    for label, config in selected:
        report = evaluator.evaluate(binary, manifest, config, expected_paths[label], verify=True)
        evaluator.print_report(report)
        reports[label] = report
        codes[label] = 0 if evaluator.passed(report) else 1
    return reports, codes


def save_results(reports, codes, json_out=None, csv_out=None):
    if len(reports) == 1:
        evaluator.write_report(next(iter(reports.values())), json_out, csv_out)
    else:
        comparison = comparison_report(reports, codes)
        print_comparison(reports)
        if json_out:
            evaluator.write_json(json_out, comparison)
        if csv_out:
            write_comparison_csv(csv_out, reports)


def main(argv=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=root / "build")
    parser.add_argument("--config", default="Debug")
    parser.add_argument("--generator")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--all-checks", action="store_true", help="run implementation checks for all three labs")
    parser.add_argument("--expected", type=Path, help="override the reference baseline for one configuration")
    add_analysis_arguments(parser)
    args = parser.parse_args(argv)
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    selected = configurations(args, parser)
    if args.expected and len(selected) != 1:
        parser.error("--expected applies to a single configuration")
    expected_paths = {}
    for label, config in selected:
        name = config["domain"] + "-intra" if args.mode == "intra" else label
        path = args.expected or root / "tests/labs/expected" / (name + ".json")
        if not path.is_file():
            parser.error("reference baseline does not exist: " + str(path))
        expected_paths[label] = path.resolve()
    manifest = args.manifest or root / "tests/labs" / ("all.json" if args.mode == "inter" else "common.json")
    try:
        labels = "^labs$" if args.all_checks else check_labels(args.domain, args.mode)
        binary = build_binary(root, args.build_dir.resolve(), args.config, args.jobs, args.generator,
                              test_labels=labels)
        reports, codes = run_evaluations(binary, selected, manifest.resolve(), expected_paths)
        save_results(reports, codes, args.json_out, args.csv_out)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("Lab test run failed: " + str(error), file=sys.stderr)
        return 1
    return 0 if all(code == 0 for code in codes.values()) else 1


if __name__ == "__main__":
    raise SystemExit(main())
