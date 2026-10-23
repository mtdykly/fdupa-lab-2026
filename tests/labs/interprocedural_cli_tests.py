#!/usr/bin/env python3
"""Interprocedural CLI contracts; never included in benchmark accuracy totals."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--domain", choices=["interval", "octagon"], required=True)
    parser.add_argument("--context-starter", action="store_true")
    parser.add_argument("--domain-starter", action="store_true")
    parser.add_argument("--implementation", choices=["reference", "starter", "student"])
    args = parser.parse_args()
    binary = args.binary.resolve()
    failures, count = [], 0
    with tempfile.TemporaryDirectory(prefix="fdulang inter ") as temporary:
        directory = Path(temporary)
        basic = directory / "basic.fdulang"
        basic.write_text("fn main(){abort;abort;}\n", encoding="utf-8")
        calls = directory / "calls.fdulang"
        calls.write_text("fn helper(x){return x;}fn main(){let y=helper(1);if(y!=1){abort;}}\n",
                         encoding="utf-8")

        def invoke(path=basic, extra=(), code=0, structured=True, mode="inter"):
            command = [str(binary), "analyze", str(path), "--domain", args.domain, "--mode", mode]
            if structured:
                command.append("--json")
            completed = subprocess.run(command + list(extra), capture_output=True, timeout=10)
            assert completed.returncode == code, (completed.returncode, completed.stderr.decode("utf-8"))
            output = completed.stdout.decode("utf-8")
            return json.loads(output) if structured and code != 64 else output

        def check(name, action):
            nonlocal count
            count += 1
            try:
                action()
                print("PASS " + name)
            except Exception as error:
                failures.append(name)
                print("FAIL " + name + ": " + str(error))

        def defaults():
            result = invoke()
            assert result["status"] == "success"
            assert result["may_abort"] == ["A1"]
            assert [item["id"] for item in result["abort_sites"]] == ["A1", "A2"]
            config = {"domain": args.domain, "mode": "inter", "solver_version": "inter-v1",
                      "implementation": args.implementation or (
                          "starter" if args.context_starter or args.domain_starter else "reference"),
                      "widen_delay": 2, "max_iterations": 1000000, "k": 1, "max_contexts": 10000}
            if args.domain == "octagon":
                config.update(domain_version="octagon-v1", max_variables=128)
            assert result["config"] == config, result
            assert result["stats"]["contexts"] == 1
            assert all(type(result["stats"][key]) is int for key in
                       ("summary_updates", "dependency_notifications"))

        def depth(k):
            result = invoke(calls, ["--k", str(k), "--max-contexts", "0", "--max-iterations", "0"])
            assert result["config"]["k"] == k
            assert result["config"]["max_contexts"] == 0
            assert result["status"] == "success"
            expected = ["A1"] if args.context_starter or args.domain_starter else []
            assert result["may_abort"] == expected, result
            assert result["stats"]["contexts"] == 2

        def limit(option, value, error_code):
            result = invoke(calls, [option, value], 3)
            assert result["status"] == "resource_limit"
            assert result["code"] == error_code, result
            assert "may_abort" not in result
            assert len(result["abort_sites"]) == 1

        def unreachable_function():
            path = directory / "unused.fdulang"
            path.write_text("fn unused(){abort;}fn main(){halt;}\n", encoding="utf-8")
            result = invoke(path)
            assert result["may_abort"] == []
            assert len(result["abort_sites"]) == 1
            assert result["stats"]["contexts"] == 1

        def deterministic():
            assert invoke(calls) == invoke(calls)

        def text():
            output = invoke(structured=False)
            assert args.domain + "/inter" in output
            assert "A1 at " in output and "A2 at " not in output

        def option_order():
            result = invoke(extra=["--mode", "intra", "--k", "0", "--mode", "inter"])
            assert result["config"]["k"] == 0

        check("inter_default_config_and_inventory", defaults)
        for k in (0, 1, 2):
            check("context_depth_" + str(k), lambda k=k: depth(k))
        check("context_limit_is_incomplete", lambda: limit("--max-contexts", "1", "analysis_contexts"))
        check("iteration_limit_is_incomplete", lambda: limit("--max-iterations", "1", "analysis_iterations"))
        check("unreachable_functions_are_not_roots", unreachable_function)
        check("deterministic_inter_output", deterministic)
        check("inter_human_readable_locations", text)
        check("final_mode_validates_context_options", option_order)
        for name, extra in [("negative_k", ["--k", "-1"]),
                            ("overflow_k", ["--k", "9" * 30]),
                            ("missing_k", ["--k"]),
                            ("negative_context_limit", ["--max-contexts", "-1"]),
                            ("overflow_context_limit", ["--max-contexts", "18446744073709551616"]),
                            ("missing_context_limit", ["--max-contexts"])]:
            check(name, lambda extra=extra: invoke(extra=extra, code=64))
        check("k_rejected_in_intra", lambda: invoke(extra=["--k", "1"], code=64, mode="intra"))
        check("context_limit_rejected_in_intra", lambda: invoke(extra=["--max-contexts", "0"], code=64, mode="intra"))
    print("Inter CLI ({}): {} passed, {} failed".format(args.domain, count - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
