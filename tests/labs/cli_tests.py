#!/usr/bin/env python3
"""Analysis command contract tests; these are not benchmark accuracy samples."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--domain", choices=["interval", "octagon"], default="interval")
    parser.add_argument("--starter", action="store_true")
    parser.add_argument("--implementation", choices=["reference", "starter", "student"])
    args = parser.parse_args()
    binary = args.binary.resolve()
    failures = []
    count = 0
    with tempfile.TemporaryDirectory(prefix="fdulang analysis ") as temporary:
        path = Path(temporary) / "program.fdulang"
        path.write_text("fn main(){abort; abort;}\n", encoding="utf-8")

        def invoke(command, code=0, structured=False):
            if command[0] == "analyze":
                command = command[:2] + ["--domain", args.domain] + command[2:]
            completed = subprocess.run([str(binary), *map(str, command)],
                                       capture_output=True, timeout=10)
            assert completed.returncode == code, (completed.returncode, completed.stderr)
            if structured:
                return json.loads(completed.stdout.decode("utf-8"))
            return completed.stdout.decode("utf-8")

        def check(name, action):
            nonlocal count
            count += 1
            try:
                action()
                print("PASS " + name)
            except Exception as error:
                failures.append(name)
                print("FAIL " + name + ": " + str(error))

        def success():
            result = invoke(["analyze", path, "--json"], structured=True)
            assert result["format_version"] == 1
            assert result["status"] == "success"
            assert result["may_abort"] == ["A1"]
            assert [site["id"] for site in result["abort_sites"]] == ["A1", "A2"]
            config = {"domain": args.domain, "mode": "intra",
                "solver_version": "intra-v1",
                "implementation": args.implementation or ("starter" if args.starter else "reference"),
                "widen_delay": 2, "max_iterations": 1000000}
            if args.domain == "octagon":
                config.update(domain_version="octagon-v1", max_variables=128)
            assert result["config"] == config
            assert result["stats"]["functions"] == 1
            assert result["stats"]["iterations"] > 0
            assert result["source"] == str(path)
            assert "abort_id" not in result

        def resource():
            result = invoke(["analyze", path, "--json", "--max-iterations", "1"], 3, True)
            assert result["status"] == "resource_limit"
            assert result["code"] == "analysis_iterations"
            assert "may_abort" not in result
            assert len(result["abort_sites"]) == 2

        def custom_config():
            result = invoke(["analyze", path, "--json", "--domain", args.domain, "--mode", "intra",
                             "--widen-delay", "0", "--max-iterations", "0"], structured=True)
            assert result["status"] == "success"
            assert result["config"]["widen_delay"] == 0
            assert result["config"]["max_iterations"] == 0

        def deterministic():
            arguments = ["analyze", path, "--json"]
            assert invoke(arguments) == invoke(arguments)

        def text_output():
            output = invoke(["analyze", path])
            assert "analysis success" in output and "A1 at " in output
            assert "A2 at " not in output

        def capacity_limit():
            large = Path(temporary) / "capacity.fdulang"
            large.write_text("fn main(){" + "".join("let x{}=0;".format(i) for i in range(129)) +
                             "abort;}\n", encoding="utf-8")
            result = invoke(["analyze", large, "--json"], 3, True)
            assert result["status"] == "resource_limit"
            assert result["code"] == "octagon_variables"
            assert "may_abort" not in result
            assert [site["id"] for site in result["abort_sites"]] == ["A1"]
            assert result["config"]["domain"] == "octagon"

        def relation_support():
            related = Path(temporary) / "relation.fdulang"
            related.write_text("fn main(){let x=0;input(x);if(x<0){halt;}if(x>10){halt;}"
                               "let y=10-x;if(x+y!=10){abort;}}\n", encoding="utf-8")
            result = invoke(["analyze", related, "--json"], structured=True)
            expected = [] if args.domain == "octagon" and not args.starter else ["A1"]
            assert result["may_abort"] == expected, result

        check("complete_inventory_and_distinct_analysis_result", success)
        check("resource_limit_is_not_a_successful_partial_result", resource)
        check("custom_configuration", custom_config)
        check("deterministic_output", deterministic)
        check("human_readable_locations", text_output)
        check("domain_specific_relation_precision", relation_support)
        if args.domain == "octagon":
            check("octagon_capacity_is_a_resource_failure", capacity_limit)
        for name, extra in [("unsupported_domain", ["--domain", "unknown-domain"]),
                            ("unsupported_mode", ["--mode", "unknown-mode"]),
                            ("negative_delay", ["--widen-delay", "-1"]),
                            ("overflow_limit", ["--max-iterations", "9" * 30]),
                            ("missing_domain", ["--domain"]),
                            ("run_limit_in_analysis", ["--max-steps", "4"]),
                            ("input_in_analysis", ["--input", "-"])]:
            check(name, lambda extra=extra: invoke(["analyze", path, *extra], 64))
        check("analysis_option_in_run", lambda: invoke(["run", path, "--domain", "interval"], 64))
        check("analysis_option_in_check", lambda: invoke(["check", path, "--widen-delay", "2"], 64))
    print("Analysis CLI ({}): {} passed, {} failed".format(args.domain, count - len(failures), len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
