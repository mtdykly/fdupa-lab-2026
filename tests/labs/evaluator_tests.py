#!/usr/bin/env python3
"""Evaluator contract checks; these are not program-analysis benchmark cases."""

import copy
from contextlib import redirect_stderr, redirect_stdout
import hashlib
import io
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import evaluate_labs as EVALUATOR
import run_lab_tests as RUNNER


class EvaluatorTests(unittest.TestCase):
    def setUp(self):
        self.config = {"domain": "interval", "mode": "intra", "widen_delay": 2,
                       "max_iterations": 1000000, "solver_version": "intra-v1"}
        self.case = {"id": "example", "source": "example.fdulang", "source_sha256": "a" * 64,
                     "aborts": [{"id": "A1", "function": "main",
                                  "location": {"offset": 12, "line": 1, "column": 13},
                                  "reachable": False},
                                 {"id": "A2", "function": "main",
                                  "location": {"offset": 20, "line": 2, "column": 1},
                                  "reachable": False}]}
        self.payload = {"format_version": 1, "source": "example.fdulang", "status": "success",
                        "config": dict(self.config, implementation="reference"),
                        "abort_sites": [{key: value for key, value in a.items() if key != "reachable"}
                                        for a in self.case["aborts"]], "may_abort": ["A1"], "stats": {}}

    def validate(self, payload=None, code=0):
        return EVALUATOR.validate_prediction(payload or self.payload, code, self.case, self.config)

    def test_valid_prediction(self):
        self.assertEqual(self.validate(), {"A1"})

    def test_octagon_config_and_prediction(self):
        args = SimpleNamespace(domain="octagon", mode="intra", widen_delay=2, max_iterations=1000000)
        config = EVALUATOR.config_for(args)
        self.assertEqual(config["domain_version"], "octagon-v1")
        self.assertEqual(config["max_variables"], 128)
        self.payload["config"] = dict(config, implementation="reference")
        self.assertEqual(EVALUATOR.validate_prediction(self.payload, 0, self.case, config), {"A1"})

    def test_octagon_capacity_config_is_checked(self):
        args = SimpleNamespace(domain="octagon", mode="intra", widen_delay=2, max_iterations=1000000)
        config = EVALUATOR.config_for(args)
        self.payload["config"] = dict(config, implementation="reference", max_variables=256)
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.validate_prediction(self.payload, 0, self.case, config)

    def test_domain_mismatch_is_rejected(self):
        self.payload["config"]["domain"] = "octagon"
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_inter_config_includes_context_controls(self):
        args = SimpleNamespace(domain="octagon", mode="inter", widen_delay=2, max_iterations=1000000,
                               k=2, max_contexts=10000)
        config = EVALUATOR.config_for(args)
        self.assertEqual(config["solver_version"], "inter-v1")
        self.assertEqual(config["k"], 2)
        self.assertEqual(config["max_contexts"], 10000)
        self.assertEqual(config["domain_version"], "octagon-v1")
        self.payload["config"] = dict(config, implementation="reference")
        self.assertEqual(EVALUATOR.validate_prediction(self.payload, 0, self.case, config), {"A1"})
        self.payload["config"]["k"] = 1
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.validate_prediction(self.payload, 0, self.case, config)

    def test_inter_baseline_cannot_be_used_for_intra(self):
        self.payload["config"].update(mode="inter", solver_version="inter-v1", k=1, max_contexts=10000)
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_invalid_json(self):
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.strict_json("not json")

    def test_duplicate_json_keys(self):
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.strict_json('{"may_abort":[],"may_abort":["A1"]}')

    def test_nonfinite_json(self):
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.strict_json('{"value":NaN}')

    def test_nonobject_json(self):
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.strict_json('[]')

    def test_unknown_abort(self):
        self.payload["may_abort"] = ["A3"]
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_duplicate_prediction(self):
        self.payload["may_abort"] = ["A1", "A1"]
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_missing_inventory(self):
        del self.payload["abort_sites"]
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_incomplete_inventory(self):
        self.payload["abort_sites"].pop()
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_duplicate_inventory(self):
        self.payload["abort_sites"].append(copy.deepcopy(self.payload["abort_sites"][0]))
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_location_mismatch(self):
        self.payload["abort_sites"][0]["location"]["column"] += 1
        # The payload shares dictionaries with the fixture; restore its ground truth separately.
        case = copy.deepcopy(self.case)
        case["aborts"][0]["location"]["column"] = 13
        with self.assertRaises(EVALUATOR.EvaluationError):
            EVALUATOR.validate_prediction(self.payload, 0, case, self.config)

    def test_non_success_status(self):
        self.payload["status"] = "resource_limit"
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_nonzero_exit(self):
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate(code=3)

    def test_wrong_config(self):
        self.payload["config"]["widen_delay"] = 3
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_boolean_is_not_version_integer(self):
        self.payload["format_version"] = True
        with self.assertRaises(EVALUATOR.EvaluationError):
            self.validate()

    def test_starter_identifier_is_valid(self):
        self.payload["config"]["implementation"] = "starter"
        self.assertEqual(self.validate(), {"A1"})

    def test_zero_denominators_are_none(self):
        result = EVALUATOR.metrics(dict(TP=0, FP=0, TN=0, FN=0))
        self.assertTrue(all(value is None for value in result.values()))

    def test_confusion_and_fpr_denominator(self):
        result = EVALUATOR.metrics(dict(TP=2, FP=1, TN=3, FN=1))
        self.assertEqual(result["fpr"], 1 / 4)
        self.assertEqual(result["fnr"], 1 / 3)
        self.assertEqual(result["precision"], 2 / 3)

    def test_same_accuracy_does_not_mean_same_predictions(self):
        first, second = {"A1"}, {"A2"}
        self.assertEqual(EVALUATOR.confusion(self.case, first), EVALUATOR.confusion(self.case, second))
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "expected.json"
            baseline = {"format_version": 1, "kind": "fdulang-lab-expected", "manifest_sha256": "b" * 64,
                        "config": dict(self.config, implementation="reference"),
                        "cases": [{"id": "example", "source_sha256": "a" * 64, "may_abort": ["A1"]}]}
            path.write_text(json.dumps(baseline), encoding="utf-8")
            expected = EVALUATOR.load_expected(path, {"cases": [self.case]}, "b" * 64, self.config)
            self.assertNotEqual(second, expected["example"])

    def test_expected_manifest_hash_mismatch(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "expected.json"
            path.write_text(json.dumps({"format_version": 1, "kind": "fdulang-lab-expected",
                                       "manifest_sha256": "a" * 64}), encoding="utf-8")
            with self.assertRaises(EVALUATOR.EvaluationError):
                EVALUATOR.load_expected(path, {"cases": [self.case]}, "b" * 64, self.config)

    def test_boolean_is_not_expected_version_integer(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "expected.json"
            baseline = {"format_version": True, "kind": "fdulang-lab-expected", "manifest_sha256": "b" * 64,
                        "config": dict(self.config, implementation="reference"),
                        "cases": [{"id": "example", "source_sha256": "a" * 64, "may_abort": ["A1"]}]}
            path.write_text(json.dumps(baseline), encoding="utf-8")
            with self.assertRaises(EVALUATOR.EvaluationError):
                EVALUATOR.load_expected(path, {"cases": [self.case]}, "b" * 64, self.config)

    def test_changed_source_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "example.fdulang"
            source.write_bytes(b"fn main() { halt; }\n")
            case = dict(self.case, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), aborts=[])
            manifest = root / "common.json"
            manifest.write_text(json.dumps({"format_version": 1, "suite": "common", "cases": [case]}), encoding="utf-8")
            self.assertEqual(len(EVALUATOR.load_manifest(manifest)["cases"]), 1)
            source.write_bytes(b"fn main() { abort; }\n")
            with self.assertRaises(EVALUATOR.EvaluationError):
                EVALUATOR.load_manifest(manifest)

    def test_negative_labels_do_not_need_witnesses(self):
        self.assertEqual(EVALUATOR.confusion(self.case, set()), dict(TP=0, FP=0, TN=2, FN=0))

    def evaluate_fake(self, callback, extra=()):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "example.fdulang"
            source.write_bytes(b"fn main() { abort; }\n")
            case = copy.deepcopy(self.case)
            case["source_sha256"] = hashlib.sha256(source.read_bytes()).hexdigest()
            manifest = root / "common.json"
            manifest.write_text(json.dumps({"format_version": 1, "suite": "test", "cases": [case]}), encoding="utf-8")
            output, expected = root / "report.json", root / "expected.json"
            argv = ["--binary", sys.executable, "--manifest", str(manifest), "--json-out", str(output)]
            argv += list(extra)
            def fake(command, timeout):
                payload = copy.deepcopy(self.payload)
                payload["source"] = str(source)
                return callback(command, timeout, payload)
            with patch.object(EVALUATOR, "run_json", side_effect=fake), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                code = EVALUATOR.main(argv)
            return code, EVALUATOR.read_json(output), expected.exists()

    def test_timeout_is_incomplete_not_a_safe_prediction(self):
        def timeout(command, limit, payload):
            raise subprocess.TimeoutExpired(command, limit)
        code, report, _ = self.evaluate_fake(timeout)
        self.assertEqual(code, 1)
        self.assertFalse(report["complete"])
        self.assertEqual(report["cases_failed"], 1)
        self.assertEqual(report["metric_scope"], "successful_cases_only")
        self.assertEqual(report["confusion"], dict(TP=0, FP=0, TN=0, FN=0))
        self.assertNotIn("may_abort", report["cases"][0])

    def test_inter_options_are_forwarded_and_recorded(self):
        def inter(command, limit, payload):
            self.assertEqual(command[command.index("--mode") + 1], "inter")
            self.assertEqual(command[command.index("--k") + 1], "0")
            self.assertEqual(command[command.index("--max-contexts") + 1], "0")
            payload["config"].update(mode="inter", solver_version="inter-v1", k=0, max_contexts=0)
            return 0, payload
        code, report, _ = self.evaluate_fake(inter, extra=["--mode", "inter", "--k", "0", "--max-contexts", "0"])
        self.assertEqual(code, 0)
        self.assertTrue(report["complete"])
        self.assertEqual(report["config"]["k"], 0)

    def test_inter_failure_is_excluded_from_accuracy(self):
        def resource(command, limit, payload):
            payload["status"] = "resource_limit"
            payload.pop("may_abort")
            return 3, payload
        code, report, _ = self.evaluate_fake(resource, extra=["--mode", "inter"])
        self.assertEqual(code, 1)
        self.assertFalse(report["complete"])
        self.assertEqual(report["confusion"], dict(TP=0, FP=0, TN=0, FN=0))

    def test_inter_expected_checks_every_configuration_field(self):
        config = dict(self.config, domain="octagon", mode="inter", solver_version="inter-v1",
                      domain_version="octagon-v1", max_variables=128, k=2, max_contexts=10000)
        baseline = {"format_version": 1, "kind": "fdulang-lab-expected", "manifest_sha256": "b" * 64,
                    "config": dict(config, implementation="reference"),
                    "cases": [{"id": "example", "source_sha256": "a" * 64, "may_abort": ["A1"]}]}
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "octagon-inter-k2.json"
            path.write_text(json.dumps(baseline), encoding="utf-8")
            result = EVALUATOR.load_expected(path, {"cases": [self.case]}, "b" * 64, config)
            self.assertEqual(result, {"example": {"A1"}})
            for field, value in (("solver_version", "inter-v2"), ("domain_version", "octagon-v2"),
                                 ("k", 1), ("max_contexts", 0), ("max_variables", 256)):
                with self.subTest(field=field), self.assertRaises(EVALUATOR.EvaluationError):
                    EVALUATOR.load_expected(path, {"cases": [self.case]}, "b" * 64,
                                            dict(config, **{field: value}))

    def test_default_manifest_depends_on_mode(self):
        for mode, name in (("intra", "common.json"), ("inter", "all.json")):
            with patch.object(EVALUATOR, "load_manifest", side_effect=EVALUATOR.EvaluationError("stop")) as load, \
                    redirect_stderr(io.StringIO()):
                self.assertEqual(EVALUATOR.main(["--binary", sys.executable, "--mode", mode]), 2)
            self.assertEqual(load.call_args.args[0], ROOT / "tests/labs" / name)

    def test_invalid_context_arguments_are_usage_errors(self):
        for extra in (["--k", "1"], ["--max-contexts", "0"],
                      ["--mode", "inter", "--k", "-1"],
                      ["--mode", "inter", "--max-contexts", "-1"]):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                EVALUATOR.main(["--binary", sys.executable, *extra])
            self.assertEqual(raised.exception.code, 2)



class ComparisonRunnerTests(unittest.TestCase):
    def setUp(self):
        self.reports = {}
        for domain in ("interval", "octagon"):
            self.reports[domain] = {"manifest_sha256": "a" * 64, "config": {"domain": domain},
                                    "complete": True, "confusion": dict(TP=1, FP=0, TN=0, FN=0),
                                    "cases": [{"id": "one", "source_sha256": "b" * 64,
                                               "status": "success", "may_abort": ["A1"],
                                               "confusion": dict(TP=1, FP=0, TN=0, FN=0)}]}

    def test_same_manifest_keeps_domain_denominators_separate(self):
        result = RUNNER.comparison_report(self.reports, {"interval": 0, "octagon": 0})
        self.assertTrue(result["complete"])
        self.assertTrue(result["passed"])
        self.assertEqual(result["domains"]["interval"]["confusion"]["TP"], 1)
        self.assertEqual(result["domains"]["octagon"]["confusion"]["TP"], 1)
        self.assertNotIn("confusion", result)

    def test_different_manifests_cannot_be_compared(self):
        self.reports["octagon"]["manifest_sha256"] = "c" * 64
        with self.assertRaises(ValueError):
            RUNNER.comparison_report(self.reports, {"interval": 0, "octagon": 0})

    def test_different_source_inventory_cannot_be_compared(self):
        self.reports["octagon"]["cases"][0]["source_sha256"] = "c" * 64
        with self.assertRaises(ValueError):
            RUNNER.comparison_report(self.reports, {"interval": 0, "octagon": 0})

    def test_failed_domain_does_not_become_a_passing_comparison(self):
        self.reports["octagon"]["complete"] = False
        result = RUNNER.comparison_report(self.reports, {"interval": 0, "octagon": 1})
        self.assertFalse(result["complete"])
        self.assertFalse(result["passed"])

    def test_same_configuration_is_not_assumed(self):
        self.reports["octagon"]["config"]["domain"] = "interval"
        with self.assertRaises(ValueError):
            RUNNER.comparison_report(self.reports, {"interval": 0, "octagon": 0})

    def test_comparison_csv_records_domain(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "comparison.csv"
            RUNNER.write_comparison_csv(path, self.reports)
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines[0].startswith("domain,"))
            self.assertTrue(lines[1].startswith("interval,"))
            self.assertTrue(lines[2].startswith("octagon,"))

    def test_inter_six_configuration_denominators_stay_separate(self):
        reports = {}
        for domain in ("interval", "octagon"):
            for k in (0, 1, 2):
                key = RUNNER.configuration_key(domain, "inter", k)
                reports[key] = copy.deepcopy(self.reports[domain])
                reports[key]["config"].update(mode="inter", k=k)
        result = RUNNER.comparison_report(reports, {key: 0 for key in reports})
        self.assertEqual(len(result["configurations"]), 6)
        self.assertNotIn("confusion", result)
        self.assertTrue(all(report["confusion"]["TP"] == 1 for report in result["configurations"].values()))
        reports["interval-inter-k2"]["config"]["k"] = 1
        with self.assertRaises(ValueError):
            RUNNER.comparison_report(reports, {key: 0 for key in reports})

    def test_comparison_requires_an_exit_code_for_every_report(self):
        with self.assertRaises(ValueError):
            RUNNER.comparison_report(self.reports, {"interval": 0})

    def test_inter_csv_records_domain_and_depth(self):
        report = copy.deepcopy(self.reports["octagon"])
        report["config"].update(mode="inter", k=2)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "comparison.csv"
            RUNNER.write_comparison_csv(path, {"octagon-inter-k2": report})
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertTrue(lines[0].startswith("domain,mode,k,configuration,"))
            self.assertTrue(lines[1].startswith("octagon,inter,2,octagon-inter-k2,"))

    def test_runner_evaluates_six_configurations_directly(self):
        def evaluate(binary, manifest, config, expected, **kwargs):
            report = copy.deepcopy(self.reports[config["domain"]])
            report["config"] = config
            report.update(cases_completed=1, cases_total=1, reference_mismatches=0,
                          metrics=EVALUATOR.metrics(report["confusion"]))
            self.assertIsNotNone(expected)
            return report
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "comparison.json"
            with patch.object(RUNNER, "build_binary", return_value=Path(sys.executable)), \
                    patch.object(EVALUATOR, "evaluate", side_effect=evaluate) as calls, \
                    patch.object(EVALUATOR, "print_report"), redirect_stdout(io.StringIO()):
                code = RUNNER.main(["--domain", "all", "--mode", "inter", "--k", "all", "--json-out", str(output)])
            self.assertEqual(code, 0)
            self.assertEqual(calls.call_count, 6)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(len(result["configurations"]), 6)

    def test_runner_context_options_require_inter(self):
        for extra in (["--k", "all"], ["--max-contexts", "0"],
                      ["--mode", "inter", "--k", "-1"], ["--mode", "inter", "--k", "bad"]):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
                RUNNER.main(extra)
            self.assertEqual(raised.exception.code, 2)

    def test_runner_checks_only_the_selected_labs(self):
        groups = {"labs-common", "lab1", "lab2", "lab3-context", "lab3-interval",
                  "lab3-octagon", "lab3-comparison"}
        cases = [
            ("interval", "intra", {"labs-common", "lab1"}),
            ("octagon", "intra", {"labs-common", "lab2"}),
            ("all", "intra", {"labs-common", "lab1", "lab2"}),
            ("interval", "inter", {"labs-common", "lab1", "lab3-context", "lab3-interval"}),
            ("octagon", "inter", {"labs-common", "lab2", "lab3-context", "lab3-octagon"}),
            ("all", "inter", groups),
        ]
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / ("fdulang.exe" if sys.platform == "win32" else "fdulang")).touch()
            for domain, mode, expected in cases:
                with self.subTest(domain=domain, mode=mode), \
                        patch.object(RUNNER.shutil, "which", side_effect=lambda name: name), \
                        patch.object(RUNNER.subprocess, "run") as commands, \
                        patch.object(RUNNER, "run_evaluations", return_value=({}, {"selected": 0})), \
                        patch.object(RUNNER, "save_results"):
                    code = RUNNER.main(["--build-dir", str(build), "--domain", domain, "--mode", mode])
                    ctest = commands.call_args_list[-1].args[0]
                    self.assertEqual(code, 0)
                    self.assertEqual(ctest[0], "ctest")
                    labels = ctest[ctest.index("-L") + 1]
                    self.assertEqual({group for group in groups if re.fullmatch(labels, group)}, expected)

    def test_all_checks_does_not_change_analysis_selection(self):
        with patch.object(RUNNER, "build_binary", return_value=Path(sys.executable)) as build, \
                patch.object(RUNNER, "run_evaluations", return_value=({}, {"selected": 0})) as evaluate, \
                patch.object(RUNNER, "save_results"):
            self.assertEqual(RUNNER.main(["--all-checks"]), 0)
        self.assertEqual(build.call_args.kwargs["test_labels"], "^labs$")
        selected = evaluate.call_args.args[1]
        self.assertEqual([(label, config["domain"], config["mode"]) for label, config in selected],
                         [("interval", "interval", "intra")])


if __name__ == "__main__":
    unittest.main(verbosity=2)
