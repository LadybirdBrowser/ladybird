#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Runs the IR tests in Tests/LibJS/JIT/ir: JavaScript programs that compile functions with the jit object, whose IR
after building and after each pass (LIBJS_JIT=dump-passes) must match the CHECK lines in their comments, in the
manner of LLVM's FileCheck.

Directives, in // comments:
  OPTIONS: <LIBJS_JIT options>  added to "on,sync,dump-passes,verify-ir"
  REQUIRES: inline-allocation   skip the test in builds whose JIT code cannot allocate cells itself (AddressSanitizer)
  CHECK-FUNCTION: <name>        the checks below are about the last code compiled for the function of that name
  CHECK-PASS: <pass>[#N]        ...in its IR after that pass (the Nth time it ran; "graph building" is before any)
  CHECK: <pattern>              a line matches, after the line the previous check matched
  CHECK-NEXT: <pattern>         the line right after the previous match matches
  CHECK-SAME: <pattern>         the rest of the previously matched line matches
  CHECK-NOT: <pattern>          no line between the previous match and the next one (or the end) matches
  CHECK-COUNT-<N>: <pattern>    exactly N lines of the IR after the pass match
Patterns are literal text, except for {{regex}}, [[NAME:regex]] (which binds NAME to what the regex matched, for the
rest of the file) and [[NAME]] (what NAME was bound to).

Usage: irtest.py [--build Build/release] [-f FILTER] [--dump]
"""

import argparse
import os
import re
import subprocess
import sys

from dataclasses import dataclass
from dataclasses import field
from pathlib import Path
from typing import Dict
from typing import List
from typing import Optional
from typing import Tuple

SOURCE_DIR = Path(__file__).resolve().parents[3]
TESTS_DIR = Path(__file__).resolve().parent / "ir"
DIRECTIVE = re.compile(
    r"//\s*(OPTIONS|REQUIRES|CHECK-FUNCTION|CHECK-PASS|CHECK-NEXT|CHECK-SAME|CHECK-NOT|CHECK-COUNT-\d+|CHECK):\s?(.*)$"
)
FUNCTION_HEADER = re.compile(r"^JIT code for (\S+?)(?: \((?:.+:)?\d+:\d+\))?:$")
PASS_HEADER = re.compile(r"^=== after (.+)$")


@dataclass
class Check:
    kind: str
    pattern: str
    line: int


@dataclass
class Section:
    function: str
    pass_name: str
    checks: List[Check] = field(default_factory=list)


def parse_test(path: Path) -> Tuple[str, List[Section]]:
    options = ""
    sections: List[Section] = []
    function = None
    for number, line in enumerate(path.read_text().splitlines(), 1):
        match = DIRECTIVE.search(line)
        if not match:
            continue
        kind, argument = match.group(1), match.group(2).rstrip()
        if kind == "OPTIONS":
            options = argument
        elif kind == "REQUIRES":
            if argument != "inline-allocation":
                raise ValueError(f"{path}:{number}: unknown requirement {argument}")
        elif kind == "CHECK-FUNCTION":
            function = argument
        elif kind == "CHECK-PASS":
            if function is None:
                raise ValueError(f"{path}:{number}: CHECK-PASS before CHECK-FUNCTION")
            sections.append(Section(function, argument))
        else:
            if not sections:
                raise ValueError(f"{path}:{number}: {kind} before CHECK-PASS")
            sections[-1].checks.append(Check(kind, argument, number))
    return options, sections


def requires_inline_allocation(path: Path) -> bool:
    return re.search(r"^//\s*REQUIRES:\s*inline-allocation\s*$", path.read_text(), re.MULTILINE) is not None


def has_inline_allocation(build: Path) -> bool:
    """Whether JIT code of this build allocates cells itself: AddressSanitizer builds give no addresses of free lists."""
    try:
        cache = (build / "CMakeCache.txt").read_text()
    except OSError:
        return True
    return re.search(r"^ENABLE_ADDRESS_SANITIZER:BOOL=ON$", cache, re.MULTILINE) is None


def compile_pattern(pattern: str, variables: Dict[str, str]) -> Tuple[re.Pattern, List[str]]:
    """A regex for a check pattern, and the names of the variables it binds, in group order."""
    regex = ""
    bound = []
    position = 0
    for match in re.finditer(r"\{\{(.*?)\}\}|\[\[(\w+):(.*?)\]\]|\[\[(\w+)\]\]", pattern):
        regex += re.escape(pattern[position : match.start()])
        if match.group(1) is not None:
            regex += f"(?:{match.group(1)})"
        elif match.group(2) is not None:
            regex += f"({match.group(3)})"
            bound.append(match.group(2))
        else:
            name = match.group(4)
            if name not in variables:
                raise ValueError(f"[[{name}]] is not bound")
            regex += re.escape(variables[name])
        position = match.end()
    regex += re.escape(pattern[position:])
    return re.compile(regex), bound


def split_dump(text: str) -> Dict[str, List[Tuple[str, List[str]]]]:
    """The pass dumps of each function's last compile: function name -> [(pass, lines)]."""
    functions: Dict[str, List[Tuple[str, List[str]]]] = {}
    current: Optional[List[Tuple[str, List[str]]]] = None
    for line in text.splitlines():
        header = FUNCTION_HEADER.match(line)
        if header:
            current = []
            functions[header.group(1)] = current
            continue
        pass_header = PASS_HEADER.match(line)
        if pass_header and current is not None:
            current.append((pass_header.group(1), []))
            continue
        if current:
            current[-1][1].append(line)
    return functions


def pass_lines(passes: List[Tuple[str, List[str]]], pass_name: str) -> Optional[List[str]]:
    name, _, occurrence = pass_name.partition("#")
    matching = [lines for pass_, lines in passes if pass_ == name]
    index = int(occurrence) - 1 if occurrence else 0
    if index >= len(matching):
        return None
    return matching[index]


def run_checks(lines: List[str], section: Section, path: Path, variables: Dict[str, str]) -> List[str]:
    failures = []
    position = 0  # The line after the previous match.
    previous_line = -1
    previous_end = 0
    pending_nots: List[Tuple[Check, re.Pattern]] = []

    def check_nots(end: int):
        for check, regex in pending_nots:
            for index in range(position, end):
                if regex.search(lines[index]):
                    failures.append(f"{path.name}:{check.line}: CHECK-NOT matched {lines[index].strip()!r}")
                    break
        pending_nots.clear()

    for check in section.checks:
        try:
            regex, bound = compile_pattern(check.pattern, variables)
        except ValueError as error:
            failures.append(f"{path.name}:{check.line}: {error}")
            return failures
        if check.kind == "CHECK-NOT":
            pending_nots.append((check, regex))
            continue
        if check.kind.startswith("CHECK-COUNT-"):
            expected = int(check.kind.split("-")[-1])
            count = sum(1 for line in lines if regex.search(line))
            if count != expected:
                failures.append(f"{path.name}:{check.line}: expected {expected} lines matching, found {count}")
            continue
        match = None
        matched_line = -1
        if check.kind == "CHECK-SAME":
            if previous_line >= 0:
                match = regex.search(lines[previous_line], previous_end)
                matched_line = previous_line
        elif check.kind == "CHECK-NEXT":
            if previous_line + 1 < len(lines):
                match = regex.search(lines[previous_line + 1])
                matched_line = previous_line + 1
        else:
            for index in range(position, len(lines)):
                match = regex.search(lines[index])
                if match:
                    matched_line = index
                    break
        if not match:
            failures.append(f"{path.name}:{check.line}: {check.kind}: {check.pattern!r} did not match")
            return failures
        check_nots(matched_line)
        for name, value in zip(bound, match.groups()):
            variables[name] = value
        previous_line, previous_end = matched_line, match.end()
        position = matched_line + 1
    check_nots(len(lines))
    return failures


def run_test(js: Path, path: Path) -> List[str]:
    options, sections = parse_test(path)
    environment = dict(os.environ)
    jit = "on,sync,dump-passes,verify-ir"
    environment["LIBJS_JIT"] = f"{jit},{options}" if options else jit
    try:
        process = subprocess.run(
            [str(js), "--expose-jit-testing", str(path)],
            env=environment,
            capture_output=True,
            timeout=120,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return [f"{path.name}: js did not finish in 120 seconds"]
    stderr = process.stderr.decode("utf-8", errors="replace")
    if process.returncode != 0:
        output = process.stdout.decode("utf-8", errors="replace")
        return [f"{path.name}: js exited with {process.returncode}: {(output + stderr)[-500:]}"]
    functions = split_dump(stderr)
    failures = []
    # NB: Variables bound in the IR after one pass may be used in the IR after a later one.
    variables: Dict[str, str] = {}
    for section in sections:
        passes = functions.get(section.function)
        if passes is None:
            failures.append(f"{path.name}: no code was compiled for {section.function}")
            continue
        lines = pass_lines(passes, section.pass_name)
        if lines is None:
            failures.append(f"{path.name}: {section.function} has no IR after {section.pass_name}")
            continue
        failures += run_checks(lines, section, path, variables)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=SOURCE_DIR / "Build/release")
    parser.add_argument("-f", "--filter", default="", help="only tests whose path contains this")
    parser.add_argument("--dump", action="store_true", help="print the IR of the tests' functions")
    arguments = parser.parse_args()
    js = arguments.build / "bin/js"

    tests = sorted(path for path in TESTS_DIR.glob("*.js") if arguments.filter in str(path))
    failed = 0
    skipped = 0
    inline_allocation = has_inline_allocation(arguments.build)
    for path in tests:
        if not inline_allocation and requires_inline_allocation(path):
            print(f"SKIP {path.relative_to(SOURCE_DIR)}: JIT code of this build cannot allocate cells")
            skipped += 1
            continue
        if arguments.dump:
            environment = dict(os.environ)
            options, _ = parse_test(path)
            environment["LIBJS_JIT"] = "on,sync,dump-passes" + (f",{options}" if options else "")
            subprocess.run([str(js), "--expose-jit-testing", str(path)], env=environment, check=False)
            continue
        failures = run_test(js, path)
        if failures:
            failed += 1
            print(f"FAIL {path.relative_to(SOURCE_DIR)}")
            for failure in failures:
                print(f"  {failure}")
        else:
            print(f"PASS {path.relative_to(SOURCE_DIR)}")
    print(f"{len(tests) - failed - skipped} of {len(tests) - skipped} IR tests passed ({skipped} skipped)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
