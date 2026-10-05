#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import difflib
import fnmatch
import json
import os
import re
import subprocess
import sys

from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ANSI_COLOR_PATTERN = re.compile(r"\x1b\[[0-9;:]*m")
SUITES = ("runtime", "ast", "bytecode")


def matches_filter(path, filters):
    # Match the runtime runner's case-insensitive substring globs. Only '*'
    # and '?' are special in AK's glob syntax.
    return not filters or any(
        fnmatch.fnmatchcase(path.as_posix().lower(), f"*{pattern.lower().replace('[', '[[]')}*") for pattern in filters
    )


def run_snapshot(file, directory, executable, suite, rebaseline):
    relative = file.relative_to(directory / "input")
    expected_file = directory / "expected" / relative.with_suffix(".txt")
    output_file = directory / "output" / relative.with_suffix(".txt")
    command = [str(executable), file.as_posix(), "--disable-ansi-colors", f"--dump-{suite}"]
    if suite == "ast":
        command.append("--parse-only")
    if file.suffix == ".mjs":
        command.append("--as-module")
    process = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    output = ANSI_COLOR_PATTERN.sub("", process.stdout).strip()
    output_file.parent.mkdir(parents=True, exist_ok=True)
    with output_file.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write(output + "\n")
    if process.returncode:
        return relative.as_posix(), "PROCESS_ERROR", f"{relative}: js exited with {process.returncode}\n{output}\n"
    if rebaseline:
        expected_file.parent.mkdir(parents=True, exist_ok=True)
        with expected_file.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(output + "\n")
        return relative.as_posix(), "REBASELINED", ""
    expected = expected_file.read_text(encoding="utf-8").strip()
    if output == expected:
        return relative.as_posix(), "PASSED", ""
    difference = "\n".join(
        difflib.unified_diff(
            expected.splitlines(),
            output.splitlines(),
            fromfile=str(expected_file),
            tofile=str(output_file),
            lineterm="",
        )
    )
    return relative.as_posix(), "FAILED", f"{relative}: {suite} does not match\n{difference}\n"


def run_snapshots(source, binaries, suite, args):
    directory = source / "Tests/LibJS" / ("AST" if suite == "ast" else "Bytecode")
    input_directory = directory / "input"
    if not input_directory.is_dir():
        raise FileNotFoundError(f"Snapshot input directory does not exist: {input_directory}")
    files = [
        file
        for file in sorted(input_directory.rglob("*"))
        if file.is_file()
        and file.suffix in (".js", ".mjs")
        and matches_filter(file.relative_to(directory / "input"), args.filter)
    ]
    executable = binaries / ("js.exe" if os.name == "nt" else "js")
    results = {}
    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        futures = [executor.submit(run_snapshot, file, directory, executable, suite, args.rebaseline) for file in files]
        for file, future in zip(files, futures):
            try:
                path, status, diagnostic = future.result()
            except (OSError, ValueError) as error:
                path = file.relative_to(input_directory).as_posix()
                status = "PROCESS_ERROR"
                diagnostic = f"{path}: {error}"
            results[path] = status
            if diagnostic:
                print(diagnostic, file=sys.stderr)
    return {"results": results, "files_total": len(files)}


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run JavaScript runtime, AST, and bytecode tests.",
        epilog="Other arguments are forwarded to the runtime runner. Use --suite runtime --help-runtime for its options.",
        allow_abbrev=False,
    )
    parser.add_argument(
        "--suite", choices=SUITES, action="append", help="Run only this suite (repeatable; default: all)"
    )
    parser.add_argument(
        "-f", "--filter", action="append", default=[], help="Only run paths matching this substring glob"
    )
    parser.add_argument("--jobs", type=int, help="Number of parallel snapshot jobs")
    parser.add_argument("--rebaseline", action="store_true", help="Update AST/bytecode expectations (default: both)")
    parser.add_argument("-j", "--json", action="store_true", help="Print results as JSON")
    parser.add_argument("--per-file", action="store_true", help="Include individual runtime test results in JSON")
    parser.add_argument("--help-runtime", action="store_true", help="Show the runtime runner's options")
    parser.add_argument("--test262-parser-tests", action="store_true", help="Run test262 parser tests (runtime only)")
    args, runtime_arguments = parser.parse_known_args(argv)
    if args.jobs is not None and args.jobs < 1:
        parser.error("--jobs must be positive")
    default_suites = ("runtime",) if args.test262_parser_tests else (("ast", "bytecode") if args.rebaseline else SUITES)
    suites = list(dict.fromkeys(args.suite or default_suites))
    if args.test262_parser_tests:
        if suites != ["runtime"]:
            parser.error("--test262-parser-tests requires only the runtime suite")
        runtime_arguments.insert(0, "--test262-parser-tests")
    if args.rebaseline and "runtime" in suites:
        parser.error("--rebaseline requires AST or bytecode suites")
    if runtime_arguments and "runtime" not in suites:
        parser.error(f"Runtime arguments require --suite runtime: {' '.join(runtime_arguments)}")
    binaries = Path(__file__).resolve().parent
    runtime = binaries / ("test-js-runtime.exe" if os.name == "nt" else "test-js-runtime")
    if args.help_runtime:
        return subprocess.run([str(runtime), "--help"], check=False).returncode
    source_directory = os.getenv("LADYBIRD_SOURCE_DIR")
    if any(suite != "runtime" for suite in suites) and source_directory is None:
        parser.error("LADYBIRD_SOURCE_DIR must be set")
    for pattern in args.filter:
        runtime_arguments.extend(("--filter", pattern))
    if args.per_file:
        runtime_arguments.append("--per-file")
    elif args.json:
        runtime_arguments.append("--json")
    # Preserve the runtime runner's output, including its existing JSON schema,
    # for callers that explicitly select it or run test262 parser tests.
    if suites == ["runtime"]:
        return subprocess.run([str(runtime), *runtime_arguments], check=False).returncode

    print_json = args.json or args.per_file
    results = {}
    failed_suites = []
    for suite in suites:
        if not print_json:
            print(f"Running {suite} tests...", flush=True)
        if suite == "runtime":
            process = subprocess.run(
                [str(runtime), *runtime_arguments], stdout=subprocess.PIPE if print_json else None, check=False
            )
            output = None
            if print_json:
                try:
                    output = json.loads(process.stdout)
                except (ValueError, UnicodeDecodeError):
                    # A crashed runner may not have produced a complete JSON document.
                    output = process.stdout.decode("utf-8", errors="replace")
            result = {"exit_code": process.returncode, "output": output}
            failed = process.returncode != 0
        else:
            assert source_directory is not None
            result = run_snapshots(Path(source_directory), binaries, suite, args)
            failed = any(status in ("FAILED", "PROCESS_ERROR") for status in result["results"].values())
            if not print_json:
                counts = Counter(result["results"].values())
                summary = ", ".join(f"{count} {status.lower()}" for status, count in sorted(counts.items()))
                print(f"{suite}: {result['files_total']} files" + (f", {summary}" if summary else ""), flush=True)
        results[suite] = result
        if failed:
            failed_suites.append(suite)
    if print_json:
        print(json.dumps({"suites": results, "failed_suites": failed_suites}))
    else:
        print(f"Suites: {len(suites) - len(failed_suites)} passed, {len(failed_suites)} failed ({', '.join(suites)})")
    return int(bool(failed_suites))


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"Unable to run JavaScript tests: {error}", file=sys.stderr)
        sys.exit(1)
