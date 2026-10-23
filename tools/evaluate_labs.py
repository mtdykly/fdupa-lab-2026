#!/usr/bin/env python3
"""Evaluate explicit-abort predictions on Lab manifests, never language tests."""

import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import sys


class EvaluationError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise EvaluationError(message)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def strict_json(text):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "duplicate JSON key: " + key)
            result[key] = value
        return result

    def constant(value):
        raise EvaluationError("non-finite JSON value: " + value)

    try:
        result = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except (ValueError, TypeError) as error:
        raise EvaluationError("invalid JSON: " + str(error)) from error
    require(isinstance(result, dict), "JSON root must be an object")
    return result


def read_json(path):
    return strict_json(path.read_text(encoding="utf-8"))


def ids(values, label):
    require(isinstance(values, list), label + " must be a list")
    require(all(isinstance(v, str) and re.fullmatch(r"A[1-9][0-9]*", v) for v in values),
            label + " contains an invalid abort ID")
    require(len(values) == len(set(values)), label + " contains duplicate abort IDs")
    return set(values)


def sort_ids(values):
    return sorted(values, key=lambda value: int(value[1:]))


def load_manifest(path):
    """Read the course manifest; full authoring checks live in release/check_manifest.py."""
    data = read_json(path)
    require(data.get("format_version") == 1, "unsupported manifest format_version")
    require(data.get("suite") and data.get("cases"), "manifest needs a suite and cases")
    try:
        for case in data["cases"]:
            require(sha256(path.parent / case["source"]) == case["source_sha256"],
                    case["id"] + ": source SHA-256 mismatch")
            ids([site["id"] for site in case["aborts"]], case["id"] + " abort inventory")
            require(all(type(site["reachable"]) is bool for site in case["aborts"]),
                    case["id"] + ": every abort needs a Boolean truth label")
    except (KeyError, TypeError) as error:
        raise EvaluationError("manifest is missing required fields") from error
    return data


def config_for(args):
    config = {"domain": args.domain, "mode": args.mode, "widen_delay": args.widen_delay,
              "max_iterations": args.max_iterations, "solver_version": args.mode + "-v1"}
    if args.mode == "inter":
        config.update(k=args.k, max_contexts=args.max_contexts)
    if args.domain == "octagon":
        config.update(domain_version="octagon-v1", max_variables=128)
    return config


def check_config(config, wanted):
    require(isinstance(config, dict), "missing analysis config")
    for key, value in wanted.items():
        require(type(config.get(key)) is type(value) and config[key] == value,
                "analysis config mismatch: " + key)
    require(config.get("implementation") in ("reference", "starter", "student"), "unknown implementation identifier")


def check_inventory(payload, case):
    inventory = payload.get("abort_sites")
    require(isinstance(inventory, list) and all(isinstance(site, dict) for site in inventory),
            "missing or invalid abort_sites inventory")
    known = ids([site.get("id") for site in inventory], "abort_sites")
    expected = {site["id"]: site for site in case["aborts"]}
    require(known == set(expected), "abort_sites inventory differs from manifest")
    for site in inventory:
        original = expected[site["id"]]
        require(site.get("function") == original["function"], "abort function differs from manifest")
        actual = site.get("location")
        require(isinstance(actual, dict), "missing abort location")
        for key in ("offset", "line", "column"):
            require(type(actual.get(key)) is int and actual[key] == original["location"][key],
                    "abort location differs from manifest: " + key)
    return known


def validate_prediction(payload, returncode, case, config, source=None):
    require(returncode == 0, "analyzer exit code is " + str(returncode))
    require(type(payload.get("format_version")) is int and payload["format_version"] == 1,
            "unsupported analyzer format_version")
    require(payload.get("status") == "success", "analyzer status is not success")
    require(isinstance(payload.get("source"), str), "missing source identifier")
    if source is not None:
        require(Path(payload["source"]).resolve() == source.resolve(), "analyzer source identifier mismatch")
    check_config(payload.get("config"), config)
    known = check_inventory(payload, case)
    predicted = ids(payload.get("may_abort"), "may_abort")
    require(predicted <= known, "may_abort contains an unknown abort ID")
    require(isinstance(payload.get("stats"), dict), "missing analysis statistics")
    return predicted


def confusion(case, predicted):
    counts = dict(TP=0, FP=0, TN=0, FN=0)
    for abort in case["aborts"]:
        actual, reported = abort["reachable"], abort["id"] in predicted
        if actual:
            counts["TP" if reported else "FN"] += 1
        else:
            counts["FP" if reported else "TN"] += 1
    return counts


def metrics(counts):
    tp, fp, tn, fn = (counts[key] for key in ("TP", "FP", "TN", "FN"))
    def ratio(numerator, denominator):
        return numerator / denominator if denominator else None
    return {"accuracy": ratio(tp + tn, tp + fp + tn + fn),
            "precision": ratio(tp, tp + fp), "recall": ratio(tp, tp + fn),
            "fpr": ratio(fp, fp + tn), "fnr": ratio(fn, tp + fn),
            "f1": ratio(2 * tp, 2 * tp + fp + fn)}


def load_expected(path, manifest, manifest_hash, config):
    data = read_json(path)
    require(type(data.get("format_version")) is int and data["format_version"] == 1
            and data.get("kind") == "fdulang-lab-expected",
            "unsupported expected-result format")
    require(data.get("manifest_sha256") == manifest_hash, "expected manifest SHA-256 mismatch")
    check_config(data.get("config"), config)
    require(data["config"]["implementation"] == "reference", "expected results must come from the reference implementation")
    raw = data.get("cases")
    require(isinstance(raw, list) and all(isinstance(item, dict) for item in raw), "invalid expected case list")
    wanted = {case["id"]: case for case in manifest["cases"]}
    expected = {}
    for item in raw:
        name = item.get("id")
        require(isinstance(name, str) and name in wanted and name not in expected, "unknown or duplicate expected case ID")
        require(item.get("source_sha256") == wanted[name]["source_sha256"], "expected source SHA-256 mismatch: " + name)
        predictions = ids(item.get("may_abort"), "expected may_abort")
        require(predictions <= {a["id"] for a in wanted[name]["aborts"]}, "unknown expected abort ID")
        require(confusion(wanted[name], predictions)["FN"] == 0, "expected baseline has a false negative")
        expected[name] = predictions
    require(set(expected) == set(wanted), "expected case inventory is incomplete")
    return expected


def run_json(command, timeout, input_data=None):
    process = subprocess.run(command, input=input_data, capture_output=True, timeout=timeout)
    payload = strict_json(process.stdout.decode("utf-8"))
    return process.returncode, payload


def verify_witnesses(binary, source, case, timeout):
    checked = 0
    for site in case["aborts"]:
        if not site["reachable"]:
            continue
        code, payload = run_json([str(binary), "run", str(source), "--json"], timeout,
                                 site["witness_input"].encode("utf-8"))
        require(code == 1 and payload.get("status") == "abort", "reachable witness did not execute an abort")
        require(type(payload.get("format_version")) is int and payload["format_version"] == 1
                and payload.get("abort_id") == site["id"],
                "reachable witness executed a different abort")
        check_inventory(payload, case)
        checked += 1
    return checked


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(json.dumps(value, ensure_ascii=False, indent=2) + "\n")


def evaluate(binary, manifest_path, config, expected_path=None, timeout=10.0, verify=False):
    """Run one configuration and return its complete report (including failed cases)."""
    binary, manifest_path = Path(binary).resolve(), Path(manifest_path).resolve()
    require(binary.is_file(), "binary does not exist")
    manifest = load_manifest(manifest_path)
    manifest_hash = sha256(manifest_path)
    expected = load_expected(expected_path, manifest, manifest_hash, config) if expected_path else None
    results, failures, mismatches, exact_truth = [], 0, 0, 0
    totals = dict(TP=0, FP=0, TN=0, FN=0)
    implementations = set()
    for case in manifest["cases"]:
        source = (manifest_path.parent / case["source"]).resolve()
        item = {"id": case["id"], "source": case["source"], "source_sha256": case["source_sha256"]}
        command = [str(binary), "analyze", str(source), "--json", "--domain", config["domain"],
                   "--mode", config["mode"], "--widen-delay", str(config["widen_delay"]),
                   "--max-iterations", str(config["max_iterations"])]
        if config["mode"] == "inter":
            command += ["--k", str(config["k"]), "--max-contexts", str(config["max_contexts"])]
        try:
            code, payload = run_json(command, timeout)
            predicted = validate_prediction(payload, code, case, config, source)
            verified = verify_witnesses(binary, source, case, timeout) if verify else 0
            implementations.add(payload["config"]["implementation"])
            counts = confusion(case, predicted)
            truth = {site["id"] for site in case["aborts"] if site["reachable"]}
            exact = predicted == truth
            exact_truth += int(exact)
            item.update(status="success", may_abort=sort_ids(predicted), truth=sort_ids(truth),
                        confusion=counts, metrics=metrics(counts), exact_truth=exact,
                        witnesses_verified=verified, analysis=payload)
            # Store portable raw output: remove the local absolute filename only.
            item["analysis"] = dict(payload, source=case["source"])
            for key in totals:
                totals[key] += counts[key]
            if expected is not None:
                item["reference_match"] = predicted == expected[case["id"]]
                item["extra_vs_reference"] = sort_ids(predicted - expected[case["id"]])
                item["missing_vs_reference"] = sort_ids(expected[case["id"]] - predicted)
                mismatches += int(not item["reference_match"])
        except (OSError, UnicodeError, subprocess.TimeoutExpired, EvaluationError) as error:
            failures += 1
            item.update(status="failed", error=str(error))
        results.append(item)
    completed = len(results) - failures
    complete = failures == 0
    report = {"format_version": 1, "kind": "fdulang-lab-evaluation", "suite": manifest["suite"],
              "manifest_sha256": manifest_hash, "config": config, "implementations": sorted(implementations),
              "complete": complete, "cases_total": len(results), "cases_completed": completed,
              "cases_failed": failures, "metric_scope": "all_cases" if complete else "successful_cases_only",
              "confusion": totals, "metrics": metrics(totals), "exact_truth_cases": exact_truth,
              "exact_truth_case_rate": exact_truth / completed if completed else None,
              "reference_comparison": expected is not None,
              "reference_mismatches": mismatches if expected is not None else None,
              "reference_match_case_rate": (completed - mismatches) / completed if expected is not None and completed else None,
              "witnesses_verified": sum(item.get("witnesses_verified", 0) for item in results), "cases": results}
    return report


def passed(report):
    return report["complete"] and report["confusion"]["FN"] == 0 and not report["reference_mismatches"]


def print_report(report):
    for case in report["cases"]:
        if case["status"] == "failed":
            print(case["id"] + ": FAILED: " + case["error"])
        else:
            print(case["id"] + ": " + (", ".join(case["may_abort"]) or "no may-abort") +
                  " | " + " ".join(key + "=" + str(value) for key, value in case["confusion"].items()))
    print("\nLab cases: {cases_completed}/{cases_total} completed; {cases_failed} failed".format(**report))
    if not report["complete"]:
        print("INCOMPLETE: metrics below cover only successful cases and are not a complete grade.")
    print(" ".join(key + "=" + str(value) for key, value in report["confusion"].items()))
    print(" ".join(key + "=" + ("N/A" if value is None else "{:.2%}".format(value))
                   for key, value in report["metrics"].items()))
    print("Exact truth sets: {exact_truth_cases}/{cases_completed}".format(**report))
    comparison = str(report["reference_mismatches"]) + " differing case(s)" if report["reference_comparison"] else "not requested"
    print("Reference comparison: " + comparison)


def write_report(report, json_out=None, csv_out=None):
    if json_out:
        write_json(json_out, report)
    if csv_out:
        csv_out.parent.mkdir(parents=True, exist_ok=True)
        with csv_out.open("w", encoding="utf-8", newline="") as stream:
            fields = ["id", "status", "may_abort", "TP", "FP", "TN", "FN", "reference_match", "error"]
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for case in report["cases"]:
                row = {key: case.get(key, "") for key in ("id", "status", "reference_match", "error")}
                row["may_abort"] = " ".join(case.get("may_abort", []))
                row.update(case.get("confusion", {}))
                writer.writerow(row)


def main(argv=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, help="default: common.json for intra, all.json for inter")
    parser.add_argument("--expected", type=Path, help="explicit reference predictions to compare; omitted means no baseline comparison")
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--csv-out", type=Path)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--domain", choices=["interval", "octagon"], default="interval")
    parser.add_argument("--mode", choices=["intra", "inter"], default="intra")
    parser.add_argument("--k", type=int, help="call-string depth, inter only (default: 1)")
    parser.add_argument("--max-contexts", type=int, help="inter only; 0 disables the limit (default: 10000)")
    parser.add_argument("--widen-delay", type=int, default=2)
    parser.add_argument("--max-iterations", type=int, default=1000000)
    parser.add_argument("--verify-witnesses", action="store_true")
    args = parser.parse_args(argv)
    if not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("--timeout must be finite and positive")
    if args.widen_delay < 0 or args.max_iterations < 0:
        parser.error("analysis limits must be nonnegative")
    if args.mode == "intra" and (args.k is not None or args.max_contexts is not None):
        parser.error("--k and --max-contexts only apply to --mode inter")
    args.k = 1 if args.k is None else args.k
    args.max_contexts = 10000 if args.max_contexts is None else args.max_contexts
    if args.k < 0 or args.max_contexts < 0:
        parser.error("context depth and limits must be nonnegative")
    default_manifest = root / "tests/labs" / ("all.json" if args.mode == "inter" else "common.json")
    binary, manifest_path = args.binary.resolve(), (args.manifest or default_manifest).resolve()
    config = config_for(args)
    try:
        report = evaluate(binary, manifest_path, config, args.expected, args.timeout, args.verify_witnesses)
        print_report(report)
        write_report(report, args.json_out, args.csv_out)
    except (OSError, ValueError) as error:
        print("Lab evaluation failed: " + str(error), file=sys.stderr)
        return 2
    return 0 if passed(report) else 1


if __name__ == "__main__":
    raise SystemExit(main())
