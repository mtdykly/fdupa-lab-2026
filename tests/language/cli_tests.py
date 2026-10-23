#!/usr/bin/env python3
"""Black-box language tool tests. This file never discovers or scores lab cases."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    root = Path(__file__).resolve().parents[2]
    passed, failures = 0, []

    with tempfile.TemporaryDirectory(prefix="fdulang language ") as temp:
        work = Path(temp)

        def invoke(arguments, expected_exit, data=b"", structured=False):
            proc = subprocess.run([str(binary)] + [str(x) for x in arguments],
                                  input=data, capture_output=True, timeout=10)
            assert proc.returncode == expected_exit, (proc.returncode, proc.stdout, proc.stderr)
            if structured:
                value = json.loads(proc.stdout.decode("utf-8"))
                assert value["format_version"] == 1
                return value
            return proc.stdout.decode("utf-8")

        def source_case(name, source, status, exit_code, data=b"", extra=(), code=None):
            path = work / (name + " [source].fdulang")
            path.write_bytes(source.encode("utf-8") if isinstance(source, str) else source)
            result = invoke(["run", path, "--json", *extra], exit_code, data, True)
            assert result["status"] == status, result
            if code:
                assert result["code"] == code, result
            assert result["source"] == str(path)
            return result

        def case(name, function):
            nonlocal passed
            try:
                function()
                passed += 1
                print("PASS " + name)
            except Exception as error:
                failures.append(name)
                print("FAIL " + name + ": " + str(error))

        case("help", lambda: invoke(["--help"], 0))
        case("missing_arguments", lambda: invoke([], 64))
        case("unknown_command", lambda: invoke(["unknown-command", "none"], 64))
        case("negative_limit", lambda: invoke(["run", "none", "--max-steps", "-1"], 64))
        case("limit_overflow", lambda: invoke(["run", "none", "--max-depth", "9" * 50], 64))
        case("check_execution_options", lambda: invoke(["check", "none", "--input", "none"], 64))
        case("unknown_option", lambda: invoke(["run", "none", "--typo"], 64))
        case("missing_source_file", lambda: invoke(["run", work / "missing", "--json"], 66, structured=True))
        case("normal", lambda: source_case("normal", "fn main() { halt; }", "normal", 0, code="halt"))
        case("unicode_source_path", lambda: source_case("\u8bed\u8a00", "fn main() { halt; }", "normal", 0))
        case("implicit_return", lambda: source_case("implicit", "fn main() {}", "normal", 0, code="main_return"))
        case("overflow", lambda: source_case("overflow", "fn main(){let x=2147483647+1-1;}",
                                             "runtime_error", 2, code="integer_overflow"))
        case("invalid_input", lambda: source_case("input", "fn main(){let x=0;input(x);}",
                                                  "runtime_error", 2, b"12x", code="invalid_input"))
        case("input_eof", lambda: source_case("eof", "fn main(){let x=0;input(x);}",
                                              "runtime_error", 2, code="input_eof"))
        case("input_range", lambda: source_case("range", "fn main(){let x=0;input(x);}",
                                                "runtime_error", 2, b"2147483648", code="input_out_of_range"))
        case("step_limit", lambda: source_case("loop", "fn main(){while(0==0){} abort;}",
                                               "resource_limit", 3, extra=("--max-steps", "20"), code="step_limit"))
        case("depth_limit", lambda: source_case("recursive", "fn main(){main();}",
                                                "resource_limit", 3, extra=("--max-depth", "3"), code="call_depth_limit"))
        case("disabled_limits", lambda: source_case("unlimited", "fn main(){return 4;}", "normal", 0,
                                                     extra=("--max-depth", "0", "--max-steps", "0")))
        case("static_check_dead_code", lambda: source_case("dead", "fn main(){halt;missing();}",
                                                           "frontend_error", 65, code="name_error"))
        case("invalid_utf8", lambda: source_case("encoding", b"fn main(){/*\xff*/halt;}",
                                                 "frontend_error", 65, code="lex_error"))

        def inventory_case():
            result = source_case("inventory", "fn unused(){abort;} fn main(){abort;abort;}", "abort", 1)
            assert result["abort_id"] == "A2"
            assert [s["id"] for s in result["abort_sites"]] == ["A1", "A2", "A3"]
            assert [s["function"] for s in result["abort_sites"]] == ["unused", "main", "main"]
        case("abort_inventory", inventory_case)

        def input_file_case():
            path = work / "input file.txt"
            path.write_text("+7 unused", encoding="ascii")
            source_case("file_input", "fn main(){let x=0;input(x);if(x!=7){abort;}}", "normal", 0,
                        extra=("--input", path))
        case("input_file", input_file_case)

        def check_and_dump():
            path = work / "check only.fdulang"
            path.write_text("fn main(){let x=0;let y=1;while(x+y<=10){}abort;}", encoding="utf-8")
            result = invoke(["check", path, "--json"], 0, structured=True)
            assert result["status"] == "checked" and result["abort_sites"][0]["id"] == "A1"
            first = invoke(["dump-cfg", path], 0)
            second = invoke(["dump-cfg", path], 0)
            assert first == second and "A1" in first and "main" in first
        case("check_and_deterministic_dump", check_and_dump)

        def missing_input():
            path = work / "missing input.fdulang"
            path.write_text("fn main(){halt;}", encoding="utf-8")
            result = invoke(["run", path, "--input", work / "absent.txt", "--json"], 66, structured=True)
            assert result["status"] == "io_error"
        case("missing_input_file", missing_input)

        for filename, data, status, exit_code in [
            ("00_minimal", b"", "normal", 0), ("01_input_loop", b"3", "normal", 0),
            ("02_relation", b"4", "normal", 0), ("03_recursion", b"4", "normal", 0),
            ("03_recursion", b"5", "abort", 1), ("04_mutual_recursion", b"4", "normal", 0),
            ("04_mutual_recursion", b"5", "abort", 1), ("05_evaluation_order", b"3 2", "normal", 0),
            ("05_evaluation_order", b"2 3", "abort", 1), ("06_termination", b"", "normal", 0),
        ]:
            def run_example(filename=filename, data=data, status=status, exit_code=exit_code):
                path = root / "examples" / (filename + ".fdulang")
                result = invoke(["run", path, "--json"], exit_code, data, True)
                assert result["status"] == status, result
            case("example_" + filename + "_" + data.decode("ascii"), run_example)

    print("Language CLI: %d passed, %d failed" % (passed, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
