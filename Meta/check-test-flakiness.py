#!/usr/bin/env python3

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path

TEST_ROOT_RELATIVE = Path("Tests") / "LibWeb"
HARNESS_STATUS_POLL_INTERVAL_SECONDS = 0.1
PROCESS_TERMINATION_TIMEOUT_SECONDS = 5


@dataclass
class LevelResult:
    level: int
    failures: int
    timed_out: bool = False


@dataclass
class CandidateReport:
    relative_path: str
    levels: list = field(default_factory=list)

    def status(self, repeat):
        if any(level.timed_out for level in self.levels):
            return "timeout"
        total_failures = sum(level.failures for level in self.levels)
        if total_failures == 0:
            return "reliable"
        total_passes = len(self.levels) * repeat - total_failures
        return "flaky" if total_passes > 0 else "fail"


def log(message):
    print(message, file=sys.stderr, flush=True)


def run_git(repo_root, *args):
    return subprocess.run(
        ["git", "-C", str(repo_root), *args],
        check=True,
        capture_output=True,
        text=True,
    ).stdout


def install_signal_handlers():
    def handle_termination(signum, _frame):
        log(f"Received signal {signum}: cleaning up...")
        sys.exit(128 + signum)

    signal.signal(signal.SIGINT, handle_termination)
    signal.signal(signal.SIGTERM, handle_termination)


def cleanup_worktree(repo_root, worktree_path, scratch_dir):
    try:
        run_git(repo_root, "worktree", "remove", "--force", "--force", str(worktree_path))
    except (subprocess.CalledProcessError, OSError):
        pass

    shutil.rmtree(scratch_dir, ignore_errors=True)

    try:
        run_git(repo_root, "worktree", "prune")
    except (subprocess.CalledProcessError, OSError):
        pass


def parse_dry_run_output(output):
    tests = set()
    for line in output.splitlines():
        match = re.match(r"^\d+/\d+:\s+(.+)$", line.strip())
        if match:
            tests.add(match.group(1).strip())
    return tests


def changed_files_in_test_root(repo_root, base_ref):
    output = run_git(repo_root, "diff", "--name-only", base_ref, "--", TEST_ROOT_RELATIVE.as_posix())
    prefix = TEST_ROOT_RELATIVE.as_posix() + "/"
    return [line[len(prefix) :] for line in output.splitlines() if line.startswith(prefix)]


def find_modified_tests(repo_root, base_ref, pr_tests):
    tests_by_input_path = {}
    for test in pr_tests:
        tests_by_input_path.setdefault(test.partition("?")[0], set()).add(test)

    modified_tests = set()
    for relative_path in changed_files_in_test_root(repo_root, base_ref):
        modified_tests.update(tests_by_input_path.get(relative_path, ()))
    return modified_tests


def dry_run_tests(test_web_binary, test_root, python_executable, results_dir):
    command = [
        str(test_web_binary),
        "--dry-run",
        "--test-path",
        str(test_root),
        "--python-executable",
        python_executable,
        "--results-dir",
        str(results_dir),
    ]
    completed = subprocess.run(command, capture_output=True, text=True, timeout=300)
    if completed.returncode != 0:
        log(completed.stdout)
        log(completed.stderr)
        raise RuntimeError(f"test-web --dry-run failed for {test_root} (exit {completed.returncode})")
    return parse_dry_run_output(completed.stdout)


def discover_candidates(args, repo_root, test_web_binary, base_worktree, scratch_dir):
    working_tree_test_root = repo_root / TEST_ROOT_RELATIVE
    base_test_root = base_worktree / TEST_ROOT_RELATIVE

    log(f"Discovering tests in working tree ({working_tree_test_root})...")
    pr_tests = dry_run_tests(
        test_web_binary, working_tree_test_root, args.python_executable, scratch_dir / "dry-run-pr"
    )
    log(f"  {len(pr_tests)} tests discovered.")

    log(f"Discovering tests in base revision ({args.base_ref})...")
    base_tests = dry_run_tests(test_web_binary, base_test_root, args.python_executable, scratch_dir / "dry-run-base")
    log(f"  {len(base_tests)} tests discovered.")

    new_tests = pr_tests - base_tests
    modified_tests = find_modified_tests(repo_root, args.base_ref, pr_tests) - new_tests
    log(f"  {len(new_tests)} new and {len(modified_tests)} modified test(s) found.")

    return sorted(new_tests) + sorted(modified_tests)


def normalize_requested_test_path(repo_root, requested_path):
    path, separator, query = requested_path.partition("?")
    path = path.replace("\\", "/")
    while path.startswith("./"):
        path = path[2:]

    test_root = (repo_root / TEST_ROOT_RELATIVE).resolve()
    requested = Path(path)
    if requested.is_absolute():
        try:
            path = requested.resolve().relative_to(test_root).as_posix()
        except ValueError:
            pass
    else:
        prefix = TEST_ROOT_RELATIVE.as_posix() + "/"
        if path.startswith(prefix):
            path = path[len(prefix) :]
        elif path == TEST_ROOT_RELATIVE.as_posix():
            path = ""

    return path.rstrip("/") + separator + query


def discover_requested_candidates(args, repo_root, test_web_binary, scratch_dir):
    test_root = repo_root / TEST_ROOT_RELATIVE
    log(f"Discovering tests in working tree ({test_root})...")
    discovered_tests = dry_run_tests(
        test_web_binary, test_root, args.python_executable, scratch_dir / "dry-run-requested"
    )
    log(f"  {len(discovered_tests)} tests discovered.")

    candidates = set()
    all_paths_matched = True
    for requested_path in args.test_paths:
        normalized_path = normalize_requested_test_path(repo_root, requested_path)
        base_path, separator, _query = normalized_path.partition("?")
        if separator:
            matches = {normalized_path} & discovered_tests
        else:
            matches = {
                test
                for test in discovered_tests
                if not base_path
                or test.partition("?")[0] == base_path
                or test.partition("?")[0].startswith(base_path + "/")
            }
        if matches:
            candidates.update(matches)
        else:
            log(f"Error: no tests matched requested path: {requested_path}")
            all_paths_matched = False

    if not all_paths_matched:
        return None

    log(f"  {len(candidates)} requested test(s) found.")
    return sorted(candidates)


def parse_concurrency_levels(spec):
    levels = []
    for level_string in spec.split(","):
        level_string = level_string.strip()
        if not level_string:
            continue
        level = int(level_string)
        if level not in levels:
            levels.append(level)
    return levels


def load_results(results_js_path):
    try:
        text = results_js_path.read_text()
        # FIXME: Having a results json file would be preferable to this approach.
        text = re.sub(r"^\s*const\s+RESULTS_DATA\s*=\s*", "", text).strip().rstrip(";").strip()
        return json.loads(text)
    except (OSError, json.JSONDecodeError):
        return None


def harness_status_has_timeout(harness_status_path):
    try:
        status = harness_status_path.read_text()
    except OSError:
        return False

    match = re.search(r"^timeout:\s+(\d+)\s*$", status, re.MULTILINE)
    return match is not None and int(match.group(1)) > 0


def terminate_process(process):
    process.terminate()
    try:
        return process.communicate(timeout=PROCESS_TERMINATION_TIMEOUT_SECONDS)
    except subprocess.TimeoutExpired:
        process.kill()
        return process.communicate()


def run_candidate(args, test_web_binary, test_root, candidate, level, deadline, results_root):
    safe_name = candidate.replace("/", "_")
    results_dir = results_root / safe_name / f"j{level}"
    command = [
        str(test_web_binary),
        "--test-path",
        str(test_root),
        "--filter",
        candidate,
        "--repeat",
        str(args.repeat),
        "--test-concurrency",
        str(level),
        "--per-test-timeout",
        str(args.per_test_timeout),
        "--python-executable",
        args.python_executable,
        "--results-dir",
        str(results_dir),
    ]

    remaining = None if deadline is None else deadline - time.monotonic()
    if remaining is not None and remaining <= 0:
        return None

    process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    detected_timeout = False
    try:
        while True:
            remaining = None if deadline is None else deadline - time.monotonic()
            if remaining is not None and remaining <= 0:
                terminate_process(process)
                log(f"  {candidate} at -j{level}: deadline reached, skipped.")
                return None

            poll_timeout = HARNESS_STATUS_POLL_INTERVAL_SECONDS
            if remaining is not None:
                poll_timeout = min(poll_timeout, remaining)

            try:
                stdout, stderr = process.communicate(timeout=poll_timeout)
                break
            except subprocess.TimeoutExpired:
                if harness_status_has_timeout(results_dir / "harness-status.txt"):
                    detected_timeout = True
                    stdout, stderr = terminate_process(process)
                    break
    except BaseException:
        if process.poll() is None:
            terminate_process(process)
        raise

    results_path = results_dir / "results.js"
    results = load_results(results_path)
    if results is None:
        if detected_timeout:
            return LevelResult(level=level, failures=1, timed_out=True)
        log(stdout)
        log(stderr)
        raise RuntimeError(f"Could not parse {results_path} for {candidate} at -j{level}")

    failures = 0
    timed_out = detected_timeout
    for entry in results.get("tests", []):
        if entry.get("result") == "Skipped":
            continue
        name = re.sub(r"^run-\d+/", "", entry.get("name", ""))
        if name == candidate or name.startswith(candidate + "@"):
            failures += 1
            timed_out = timed_out or entry.get("result") == "Timeout"
    return LevelResult(level=level, failures=failures, timed_out=timed_out)


def format_levels(report, repeat):
    return "; ".join(f"-j{result.level}: {result.failures}/{repeat} failed" for result in report.levels)


def write_summary(reports, skipped, levels, repeat):
    flaky = [report for report in reports if report.status(repeat) == "flaky"]
    failing = [report for report in reports if report.status(repeat) == "fail"]
    timed_out = [report for report in reports if report.status(repeat) == "timeout"]

    lines = [
        f"Checked {len(reports)} candidate test(s) at parallelism levels {levels}: "
        f"{len(flaky)} flaky, {len(failing)} always failing, {len(timed_out)} timed out.",
        "",
    ]

    if flaky:
        lines.append("Flaky tests")
        lines.append("")
        for report in flaky:
            lines.append(f"- {report.relative_path} at {format_levels(report, repeat)}")
        lines.append("")

    if failing:
        lines.append("Tests that always fail")
        lines.append("")
        for report in failing:
            lines.append(f"- {report.relative_path}")
        lines.append("")

    if timed_out:
        lines.append("Tests that timed out")
        lines.append("")
        for report in timed_out:
            timeout_levels = ", ".join(f"-j{result.level}" for result in report.levels if result.timed_out)
            lines.append(f"- {report.relative_path} at {timeout_levels}")
        lines.append("")

    if skipped:
        lines.append("Not checked (time budget exhausted)")
        lines.append("")
        for candidate in skipped:
            lines.append(f"- {candidate}")
        lines.append("")

    print("\n".join(lines))
    return flaky + failing + timed_out


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-web-binary", required=True, type=Path, help="Path to the built test-web binary")
    parser.add_argument("--base-ref", default="HEAD", help="Base revision to diff against (default: HEAD)")
    parser.add_argument(
        "--source-dir",
        type=Path,
        default=Path(os.environ.get("LADYBIRD_SOURCE_DIR", ".")),
        help="Repository root of the working tree (default: LADYBIRD_SOURCE_DIR or .)",
    )
    parser.add_argument(
        "--repeat", type=int, default=100, help="Number of times to repeat each candidate (default: 100)"
    )
    parser.add_argument(
        "--concurrency-levels",
        default=f"1,{os.cpu_count() or 1}",
        help="Comma-separated --test-concurrency levels to run (default: 1,<nproc>)",
    )
    parser.add_argument(
        "--per-test-timeout", type=int, default=30, help="Per-test timeout in seconds passed to test-web (default: 30)"
    )
    parser.add_argument(
        "--deadline-seconds", type=int, default=0, help="Overall wall-clock budget in seconds (default: 0, no limit) "
    )
    parser.add_argument(
        "--python-executable", default="python3", help="Path to python3 for test-web (default: python3)"
    )
    parser.add_argument(
        "test_paths",
        nargs="*",
        metavar="TEST_PATH",
        help="Specific test files or directories to check instead of discovering new and modified tests",
    )
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    install_signal_handlers()

    repo_root = args.source_dir.resolve()
    test_web_binary = args.test_web_binary.resolve()
    if not test_web_binary.exists():
        log(f"Error: test-web binary not found at {test_web_binary}")
        return 2

    levels = parse_concurrency_levels(args.concurrency_levels)
    if not levels:
        log("Error: no valid concurrency levels specified")
        return 2

    deadline = None if args.deadline_seconds <= 0 else time.monotonic() + args.deadline_seconds

    scratch_dir = Path(tempfile.mkdtemp(prefix="flaky-tests-check-")).resolve()
    if args.test_paths:
        try:
            candidates = discover_requested_candidates(args, repo_root, test_web_binary, scratch_dir)
        finally:
            shutil.rmtree(scratch_dir, ignore_errors=True)
        if candidates is None:
            return 2
    else:
        base_worktree_path = scratch_dir / "base-worktree"
        try:
            log(f"Adding base worktree for {args.base_ref}...")
            run_git(repo_root, "worktree", "add", "--detach", str(base_worktree_path), args.base_ref)
            candidates = discover_candidates(args, repo_root, test_web_binary, base_worktree_path, scratch_dir)
        finally:
            cleanup_worktree(repo_root, base_worktree_path, scratch_dir)

    if not candidates:
        log("No new or modified tests: nothing to check.")
        return 0

    log(f"{len(candidates)} candidate test(s) to check at parallelism levels {levels}.")

    skipped = []
    pr_test_root = repo_root / TEST_ROOT_RELATIVE
    reports = []
    results_root = Path(tempfile.mkdtemp(prefix="flaky-tests-results-"))
    try:
        for index, candidate in enumerate(candidates):
            if deadline is not None and time.monotonic() >= deadline:
                skipped.extend(candidates[index:])
                break
            log(f"Checking {candidate} ({args.repeat}x each at -j{','.join(map(str, levels))})...")
            report = CandidateReport(relative_path=candidate)
            saw_pass = False
            saw_fail = False
            for level in levels:
                level_result = run_candidate(
                    args, test_web_binary, pr_test_root, candidate, level, deadline, results_root
                )
                if level_result is None:
                    break
                report.levels.append(level_result)
                saw_pass = saw_pass or level_result.failures < args.repeat
                saw_fail = saw_fail or level_result.failures > 0
                if level_result.timed_out or (saw_pass and saw_fail):
                    break
            if not report.levels:
                skipped.append(candidate)
                continue
            reports.append(report)
            status = report.status(args.repeat)
            if status == "flaky":
                log(f"  FLAKY: {candidate} ({format_levels(report, args.repeat)})")
            elif status == "fail":
                log(f"  FAIL: {candidate}")
            elif status == "timeout":
                log(f"  TIMEOUT: {candidate}")
    finally:
        shutil.rmtree(results_root, ignore_errors=True)

    problems = write_summary(reports, skipped, levels, args.repeat)

    if skipped:
        log(f"Warning: {len(skipped)} candidate(s) were not checked due to the time budget.")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
