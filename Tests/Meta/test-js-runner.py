#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import contextlib
import io
import json
import os
import runpy
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path
from pathlib import PureWindowsPath
from unittest.mock import MagicMock
from unittest.mock import patch

MODULE = runpy.run_path(str(Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Tests/LibJS/test-js.py"))
MAIN = MODULE["main"]
RUN_PROCESS = subprocess.run


class TestJavaScriptRunner(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.binaries = self.root / "bin"
        self.binaries.mkdir()
        for suite in ("AST", "Bytecode"):
            directory = self.root / "Tests/LibJS" / suite
            (directory / "input/nested").mkdir(parents=True)
            (directory / "expected/nested").mkdir(parents=True)
            (directory / "input/nested/example.mjs").write_text("", encoding="utf-8")
            (directory / "expected/nested/example.txt").write_text("snapshot\n", encoding="utf-8")
        environment = patch.dict(os.environ, {"LADYBIRD_SOURCE_DIR": str(self.root)})
        environment.start()
        self.addCleanup(environment.stop)
        script = patch.dict(MAIN.__globals__, {"__file__": str(self.binaries / "test-js.py")})
        script.start()
        self.addCleanup(script.stop)
        process = patch.object(MODULE["subprocess"], "run", side_effect=self.run_process)
        self.process = process.start()
        self.addCleanup(process.stop)
        self.failed_suite = None
        self.calls = []

    def run_process(self, command, **kwargs):
        if Path(command[0]).stem == "test-js-runtime":
            suite = "runtime"
            output = b'{"results": {"tests": {"passed": 3}}}'
        else:
            suite = "ast" if "--dump-ast" in command else "bytecode"
            output = "snapshot\n"
            self.assertIn("--as-module", command)
            self.assertEqual("--parse-only" in command, suite == "ast")
        self.calls.append(suite)
        return subprocess.CompletedProcess(command, 1 if suite == self.failed_suite else 0, stdout=output)

    def invoke(self, *arguments):
        output = io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
            status = MAIN(list(arguments))
        return status, output.getvalue()

    def test_default_runs_all_suites_and_continues_after_failures(self):
        for failed_suite in (None, "runtime", "ast", "bytecode"):
            with self.subTest(failed_suite=failed_suite):
                self.failed_suite = failed_suite
                self.calls.clear()
                status, output = self.invoke("--json")
                self.assertEqual(status, int(failed_suite is not None))
                self.assertEqual(self.calls, ["runtime", "ast", "bytecode"])
                results = json.loads(output)
                self.assertEqual(results["failed_suites"], [failed_suite] if failed_suite else [])
                self.assertEqual(set(results["suites"]), {"runtime", "ast", "bytecode"})

    def test_runtime_selection_preserves_arguments_output_and_exit_code(self):
        self.process.return_value = subprocess.CompletedProcess([], 7)
        self.process.side_effect = None
        status, output = self.invoke(
            "--suite", "runtime", "--per-file", "--collect-often", "external root", "-f", "array"
        )
        self.assertEqual(status, 7)
        self.assertEqual(output, "")
        command = self.process.call_args.args[0]
        self.assertEqual(command[1:], ["--collect-often", "external root", "--filter", "array", "--per-file"])
        self.assertNotIn("stdout", self.process.call_args.kwargs)

    def test_test262_parser_invocation_preserves_runtime_json(self):
        status, output = self.invoke("--test262-parser-tests", "external root", "--json")
        self.assertEqual(status, 0)
        self.assertEqual(output, "")
        self.assertEqual(self.calls, ["runtime"])
        self.assertEqual(self.process.call_args.args[0][1:], ["--test262-parser-tests", "external root", "--json"])
        self.assertNotIn("stdout", self.process.call_args.kwargs)

    def test_snapshot_filters_and_repeated_suite_selection(self):
        status, output = self.invoke("--suite", "ast", "--suite", "ast", "-f", "NESTED/*EXAMPLE", "--json")
        self.assertEqual(status, 0)
        self.assertEqual(self.calls, ["ast"])
        self.assertEqual(json.loads(output)["suites"]["ast"]["files_total"], 1)
        self.calls.clear()
        status, output = self.invoke("--suite", "bytecode", "-f", "no-match", "--json")
        self.assertEqual(status, 0)
        self.assertEqual(self.calls, [])
        self.assertEqual(json.loads(output)["suites"]["bytecode"]["files_total"], 0)

    def test_mismatch_reports_failure_and_writes_actual_output(self):
        expected = self.root / "Tests/LibJS/AST/expected/nested/example.txt"
        expected.write_text("old snapshot\n", encoding="utf-8")
        status, output = self.invoke("--suite", "ast", "--json")
        self.assertEqual(status, 1)
        self.assertEqual(json.loads(output)["suites"]["ast"]["results"]["nested/example.mjs"], "FAILED")
        self.assertEqual(expected.read_text(), "old snapshot\n")
        self.assertEqual((self.root / "Tests/LibJS/AST/output/nested/example.txt").read_text(), "snapshot\n")

    def test_crlf_subprocess_output_is_normalized(self):
        def run_crlf_process(command, **kwargs):
            return RUN_PROCESS([sys.executable, "-c", "import os; os.write(1, b'first\\r\\nsecond\\r\\n')"], **kwargs)

        self.process.side_effect = run_crlf_process
        for suite, directory_name in (("ast", "AST"), ("bytecode", "Bytecode")):
            directory = self.root / "Tests/LibJS" / directory_name
            expected = directory / "expected/nested/example.txt"
            actual = directory / "output/nested/example.txt"
            for rebaseline in (False, True):
                with self.subTest(suite=suite, rebaseline=rebaseline):
                    expected.write_bytes(b"old snapshot\n" if rebaseline else b"first\nsecond\n")
                    _, status, diagnostic = MODULE["run_snapshot"](
                        directory / "input/nested/example.mjs", directory, sys.executable, suite, rebaseline
                    )
                    self.assertEqual(status, "REBASELINED" if rebaseline else "PASSED", diagnostic)
                    self.assertEqual(actual.read_bytes(), b"first\nsecond\n")
                    self.assertEqual(expected.read_bytes(), b"first\nsecond\n")

    def test_missing_expectation_does_not_skip_other_suites(self):
        (self.root / "Tests/LibJS/AST/expected/nested/example.txt").unlink()
        status, output = self.invoke("--json")
        self.assertEqual(status, 1)
        self.assertEqual(self.calls, ["runtime", "ast", "bytecode"])
        self.assertEqual(json.loads(output)["failed_suites"], ["ast"])

    def test_rebaseline_updates_snapshots_but_never_crash_output(self):
        expected = self.root / "Tests/LibJS/AST/expected/nested/example.txt"
        expected.write_text("old snapshot\n", encoding="utf-8")
        self.failed_suite = "ast"
        status, _ = self.invoke("--rebaseline")
        self.assertEqual(status, 1)
        self.assertEqual(self.calls, ["ast", "bytecode"])
        self.assertEqual(expected.read_text(), "old snapshot\n")
        self.failed_suite = None
        status, _ = self.invoke("--suite", "ast", "--rebaseline")
        self.assertEqual(status, 0)
        self.assertEqual(expected.read_text(), "snapshot\n")

    def test_invalid_options_do_not_run_tests(self):
        for arguments in (
            ("--jobs", "0"),
            ("--test262-parser-tests", "--suite", "ast"),
            ("--test262-parser-tests", "--rebaseline"),
            ("--suite", "runtime", "--rebaseline"),
            ("--suite", "ast", "--collect-often"),
        ):
            with self.subTest(arguments=arguments), self.assertRaises(SystemExit) as error:
                self.invoke(*arguments)
            self.assertEqual(error.exception.code, 2)
        self.assertEqual(self.calls, [])

    def test_windows_paths_match_posix_filters(self):
        matches = MODULE["matches_filter"]
        path = PureWindowsPath("nested", "example.mjs")
        self.assertTrue(matches(path, ["NESTED/*EXAMPLE"]))
        self.assertFalse(matches(path, ["other/*"]))

    def test_windows_source_paths_are_passed_with_posix_separators(self):
        directory = self.root / "Tests/LibJS/Bytecode"
        windows_path = PureWindowsPath("C:/checkout/Tests/LibJS/Bytecode/input/nested/example.mjs")
        file = MagicMock()
        file.configure_mock(**{"__str__.return_value": str(windows_path)})
        file.as_posix.return_value = windows_path.as_posix()
        file.relative_to.return_value = Path("nested/example.mjs")
        file.suffix = ".mjs"
        _, status, diagnostic = MODULE["run_snapshot"](file, directory, self.binaries / "js", "bytecode", False)
        self.assertEqual(status, "PASSED", diagnostic)
        self.assertEqual(self.process.call_args.args[0][1], windows_path.as_posix())

    def test_filter_treats_brackets_literally(self):
        matches = MODULE["matches_filter"]
        self.assertTrue(matches(Path("A[b].mjs"), ["a[b]"]))
        self.assertFalse(matches(Path("ab.mjs"), ["a[b]"]))
        self.assertTrue(matches(Path("nested/example.mjs"), ["no-match", "EX?MPLE"]))


if __name__ == "__main__":
    unittest.main()
