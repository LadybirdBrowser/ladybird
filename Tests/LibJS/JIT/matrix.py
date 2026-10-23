#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Runs JavaScript test suites under a matrix of JIT stress modes and compares every result with the interpreter's.

Suites:
  test-js   The LibJS runtime tests (Tests/LibJS/Runtime), one process per file, each test run LIBJS_TEST_REPEAT
            times so that the functions it calls get compiled with feedback. The per-test results must match the
            interpreter's with the same repeat count.
  test262   The whole test262 suite through libjs-test262's main.py; the per-file results must match the
            interpreter's.
  programs  Every .js file (and .mjs file, as a module) in the --programs directories (Tests/LibJS/JIT/programs by
            default), run with js; the exit code and the output must match the interpreter's (lines with timings are
            ignored).

Every mismatch is printed with a command that reproduces it. The exit code is 1 if there were any.
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys
import time

from pathlib import Path
from typing import Dict
from typing import List
from typing import Optional
from typing import Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from machine import wait_until_machine_is_free  # noqa: E402
from modes import INTERPRETER  # noqa: E402
from modes import Mode  # noqa: E402
from modes import modes_for  # noqa: E402

SOURCE_DIR = Path(__file__).resolve().parents[3]
TIMING_LINE = re.compile(r"\btimes?\b|[0-9] ?ms\b|\bscore\b|[0-9]+\.[0-9]+ ?s\b", re.IGNORECASE)
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")


def environment_for(mode: Mode, seed: int, extra: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    environment = dict(os.environ)
    environment.pop("LIBJS_JIT", None)
    environment["LADYBIRD_SOURCE_DIR"] = str(SOURCE_DIR)
    if mode.jit:
        environment["LIBJS_JIT"] = mode.environment_value(seed)
    if extra:
        environment.update(extra)
    return environment


def command_line(environment_changes: Dict[str, str], command: List, cwd: Optional[Path] = None) -> str:
    """A shell command line that runs `command` with the environment changes."""
    parts = [f"{name}={value}" for name, value in environment_changes.items() if value]
    parts += [str(part) for part in command]
    line = " ".join(parts)
    return f"(cd {cwd} && {line})" if cwd else line


class Suite:
    name = ""

    def __init__(self, arguments):
        self.arguments = arguments

    def run(self, mode: Mode) -> Dict[str, str]:
        raise NotImplementedError

    def repro(self, mode: Mode, key: str) -> str:
        raise NotImplementedError


class TestJsSuite(Suite):
    name = "test-js"

    def __init__(self, arguments):
        super().__init__(arguments)
        self.root = SOURCE_DIR / "Tests/LibJS/Runtime"
        self.runner = arguments.build / "bin/test-js-runtime"
        self.files = sorted(
            path
            for path in self.root.rglob("*.js")
            if path.name != "test-common.js" and all(f in str(path) for f in arguments.filter)
        )

    def environment_changes(self, mode: Mode) -> Dict[str, str]:
        return {
            "LIBJS_JIT": mode.environment_value(self.arguments.seed),
            "LIBJS_TEST_REPEAT": str(self.arguments.repeat),
        }

    def run_file(self, mode: Mode, path: Path) -> Dict[str, str]:
        wait_until_machine_is_free()
        relative = path.relative_to(self.root).as_posix()
        environment = environment_for(mode, self.arguments.seed, {"LIBJS_TEST_REPEAT": str(self.arguments.repeat)})
        try:
            process = subprocess.run(
                [self.runner, "--per-file", "-f", path],
                env=environment,
                capture_output=True,
                timeout=self.arguments.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return {f"{relative}/(process)": "TIMEOUT"}
        results = {}
        try:
            output = json.loads(process.stdout.decode("utf-8", errors="replace"))
            for test, result in output.get("results", {}).items():
                results[test] = result
        except json.JSONDecodeError:
            pass
        # NB: test-js exits with 1 when a test fails, but a crash is a signal or another exit code.
        if process.returncode not in (0, 1) or not results and process.returncode != 0:
            stderr = process.stderr.decode("utf-8", errors="replace").strip().splitlines()
            reason = stderr[-1][:200] if stderr else ""
            results[f"{relative}/(process)"] = f"CRASH {process.returncode} {reason}"
        return results

    def run(self, mode: Mode) -> Dict[str, str]:
        results = {}
        with concurrent.futures.ThreadPoolExecutor(self.arguments.jobs) as executor:
            for file_results in executor.map(lambda path: self.run_file(mode, path), self.files):
                results.update(file_results)
        return results

    def repro(self, mode: Mode, key: str) -> str:
        relative = key.split(".js/", 1)[0] + ".js"
        return command_line(
            self.environment_changes(mode) | {"LADYBIRD_SOURCE_DIR": str(SOURCE_DIR)},
            [self.runner, "-f", self.root / relative],
        )


class Test262Suite(Suite):
    name = "test262"

    def __init__(self, arguments):
        super().__init__(arguments)
        self.directory = arguments.test262
        self.runner = arguments.build / "bin/test262-runner"
        # NB: The runner scripts run with this Python, which needs their requirements.
        self.python = Path(sys.executable)
        self.pattern = arguments.test262_pattern

    def run(self, mode: Mode) -> Dict[str, str]:
        wait_until_machine_is_free()
        output = self.arguments.results / f"test262-{mode.name}.json"
        output.unlink(missing_ok=True)
        command = [
            self.python,
            self.directory / "main.py",
            "-j",
            self.runner,
            "-c",
            str(self.arguments.jobs),
            "--per-file",
            output,
            "-s",
        ]
        if self.pattern:
            command += ["-p", self.pattern]
        subprocess.run(
            command,
            cwd=self.directory,
            env=environment_for(mode, self.arguments.seed),
            stdout=subprocess.DEVNULL,
            check=False,
        )
        try:
            return json.loads(output.read_text())["results"]
        except (OSError, json.JSONDecodeError, KeyError):
            return {"(test262)": "NO RESULTS"}

    def repro(self, mode: Mode, key: str) -> str:
        return command_line(
            {"LIBJS_JIT": mode.environment_value(self.arguments.seed)},
            [self.python, "main.py", "-j", self.runner, "-v", "-p", key],
            cwd=self.directory,
        )


class ProgramsSuite(Suite):
    name = "programs"

    def __init__(self, arguments):
        super().__init__(arguments)
        self.js = arguments.build / "bin/js"
        directories = arguments.programs or [SOURCE_DIR / "Tests/LibJS/JIT/programs"]
        self.files = sorted(
            path
            for directory in directories
            for pattern in ("*.js", "*.mjs")
            for path in Path(directory).rglob(pattern)
            if all(f in str(path) for f in arguments.filter)
        )

    def run_file(self, mode: Mode, path: Path) -> tuple[str, str]:
        wait_until_machine_is_free()
        try:
            process = subprocess.run(
                self.command(path),
                cwd=path.parent,
                env=environment_for(mode, self.arguments.seed),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=self.arguments.timeout,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return str(path), "TIMEOUT"
        text = ANSI_ESCAPE.sub("", process.stdout.decode("utf-8", errors="replace"))
        lines = [line for line in text.splitlines() if not TIMING_LINE.search(line)]
        return str(path), f"exit {process.returncode}\n" + "\n".join(lines)

    def run(self, mode: Mode) -> Dict[str, str]:
        with concurrent.futures.ThreadPoolExecutor(self.arguments.jobs) as executor:
            return dict(executor.map(lambda path: self.run_file(mode, path), self.files))

    def repro(self, mode: Mode, key: str) -> str:
        path = Path(key)
        return command_line(
            {"LIBJS_JIT": mode.environment_value(self.arguments.seed)},
            self.command(path),
            cwd=path.parent,
        )

    def command(self, path: Path) -> List:
        """The js command line that runs the program at `path`, as a module if it is a .mjs file."""
        module = ["--as-module"] if path.suffix == ".mjs" else []
        return [self.js, "--expose-jit-testing", *module, path.name]


SUITES = {suite.name: suite for suite in (TestJsSuite, Test262Suite, ProgramsSuite)}

KNOWN_MISMATCHES = Path(__file__).resolve().parent / "known-mismatches.txt"


def load_known_mismatches() -> Dict[Tuple[str, str], str]:
    """The mismatches of open bugs, which do not fail a run: (suite, key) -> the bug, from lines of
    "<suite> <key> # <bug>" in known-mismatches.txt."""
    known = {}
    for line in KNOWN_MISMATCHES.read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        entry, _, bug = line.partition(" # ")
        suite, _, key = entry.partition(" ")
        known[(suite, key.strip())] = bug.strip()
    return known


def describe_difference(expected: str, actual: str) -> str:
    if "\n" not in expected and "\n" not in actual:
        return f"interpreter: {expected}, got: {actual}"
    expected_lines, actual_lines = expected.splitlines(), actual.splitlines()
    for index, (expected_line, actual_line) in enumerate(zip(expected_lines, actual_lines)):
        if expected_line != actual_line:
            return f"line {index + 1}: interpreter: {expected_line[:160]!r}, got: {actual_line[:160]!r}"
    return f"interpreter printed {len(expected_lines)} lines, got {len(actual_lines)}"


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--build", type=Path, default=SOURCE_DIR / "Build/release", help="build directory")
    parser.add_argument("--modes", default="fast", help="comma-separated modes and tiers (see modes.py)")
    parser.add_argument("--suites", default="test-js,programs", help="comma-separated suites: " + ", ".join(SUITES))
    parser.add_argument("-j", "--jobs", type=int, default=4, help="processes to run at once")
    parser.add_argument("--seed", type=int, default=1, help="seed of the modes that make random choices")
    parser.add_argument("--repeat", type=int, default=3, help="how often test-js runs each test")
    parser.add_argument("--timeout", type=int, default=300, help="seconds per test file or program")
    parser.add_argument("-f", "--filter", action="append", default=[], help="only paths containing this")
    parser.add_argument("--programs", action="append", type=Path, help="directories of programs to run")
    parser.add_argument("--test262", type=Path, help="checkout of libjs-test262, for the test262 suite")
    parser.add_argument("--test262-pattern", help="glob of test262 tests to run (default: all)")
    parser.add_argument("--results", type=Path, help="directory for the results (default: a new one in /tmp)")
    parser.add_argument(
        "--coverage", action="store_true", help="collect what compiled code contained and did, and report it"
    )
    arguments = parser.parse_args()
    if "test262" in arguments.suites.split(",") and arguments.test262 is None:
        parser.error("the test262 suite needs --test262")

    arguments.build = arguments.build.resolve()
    if arguments.test262 is not None:
        arguments.test262 = arguments.test262.resolve()
    if arguments.results is None:
        arguments.results = Path(f"/tmp/jit-matrix-{os.getpid()}")
    arguments.results = arguments.results.resolve()
    arguments.results.mkdir(parents=True, exist_ok=True)
    modes = modes_for(arguments.modes.split(","))
    if arguments.coverage:
        coverage = arguments.results / "coverage"
        modes = [Mode(mode.name, f"{mode.jit},coverage={coverage / mode.name}", mode.description) for mode in modes]
    suites = [SUITES[name](arguments) for name in arguments.suites.split(",")]

    known = load_known_mismatches()
    mismatch_count = 0
    known_seen = set()
    known_run = set()
    summary = {}
    for suite in suites:
        start = time.monotonic()
        expected = suite.run(INTERPRETER)
        print(f"{suite.name} [interpreter]: {len(expected)} results in {time.monotonic() - start:.1f} s", flush=True)
        summary[suite.name] = {"interpreter": {"results": len(expected)}}
        if "(test262)" in expected:
            print(f"{suite.name} [interpreter]: no results, so nothing to compare with")
            return 1
        for mode in modes:
            start = time.monotonic()
            actual = suite.run(mode)
            elapsed = time.monotonic() - start
            mismatches = []
            for key in sorted(set(expected) | set(actual)):
                expected_result = expected.get(key, "MISSING")
                actual_result = actual.get(key, "MISSING")
                if expected_result != actual_result:
                    mismatches.append((key, expected_result, actual_result))
            print(
                f"{suite.name} [{mode.name}]: {len(actual)} results, {len(mismatches)} mismatches in {elapsed:.1f} s",
                flush=True,
            )
            known_run |= {(suite.name, key) for key in expected if (suite.name, key) in known}
            for key, expected_result, actual_result in mismatches:
                if (suite.name, key) in known:
                    known_seen.add((suite.name, key))
                    print(f"  KNOWN ({known[(suite.name, key)]}) {key}")
                    continue
                print(f"  MISMATCH {key}: {describe_difference(expected_result, actual_result)}")
                print(f"    repro: {suite.repro(mode, key)}")
                mismatch_count += 1
            summary[suite.name][mode.name] = {
                "jit": mode.environment_value(arguments.seed),
                "results": len(actual),
                "mismatches": [{"key": key, "interpreter": e, "actual": a} for key, e, a in mismatches],
                "seconds": round(elapsed, 1),
            }
            (arguments.results / "summary.json").write_text(json.dumps(summary, indent=2))

    if arguments.coverage:
        subprocess.run(
            [
                sys.executable,
                Path(__file__).parent / "coverage.py",
                arguments.results / "coverage",
                "--json",
                arguments.results / "coverage.json",
            ],
            check=False,
        )
    fixed = sorted(known_run - known_seen)
    for suite_name, key in fixed:
        print(
            f"FIXED? {suite_name} {key} did not mismatch in these modes; remove it from known-mismatches.txt if fixed"
        )
    print(f"{mismatch_count} new mismatches, {len(known_seen)} known; results in {arguments.results}")
    return 1 if mismatch_count else 0


if __name__ == "__main__":
    sys.exit(main())
