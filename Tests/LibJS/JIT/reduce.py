#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Reduces a JavaScript program that behaves differently under a JIT mode than in the interpreter.

The program keeps "failing the same way" while lines are removed: it crashes with the same signature, or its output
still differs from the interpreter's. Lines are removed in chunks of halving size (delta debugging), then blocks are
unwrapped (an `if`, loop or `try` replaced by its body), until nothing more can go.

Usage: reduce.py program.js --mode warm [--build Build/release] [-o reduced.js]
"""

import argparse
import os
import re
import subprocess
import sys

from dataclasses import dataclass
from pathlib import Path
from typing import Callable
from typing import List
from typing import Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))

from machine import wait_until_machine_is_free  # noqa: E402
from modes import INTERPRETER  # noqa: E402
from modes import MODES_BY_NAME  # noqa: E402
from modes import Mode  # noqa: E402

SOURCE_DIR = Path(__file__).resolve().parents[3]
PANIC_LOCATION = re.compile(r"panicked at ([^\s:]+:\d+)")
ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
NUMBERED_NAME = re.compile(r"\b(v|b|fs|s|r|l|a)[0-9]+\b|\b[0-9]+\b")


@dataclass
class Run:
    exit_code: int
    output: str
    timed_out: bool

    def crash_signature(self) -> Optional[str]:
        """What identifies the crash, if the run crashed: where Rust panicked, or the signal."""
        match = PANIC_LOCATION.search(self.output)
        if match:
            # NB: The verifiers panic in one place for every invariant, so the first violation they name tells
            #     failures apart, with its value, block and frame state numbers left out.
            lines = self.output[match.end() :].splitlines()[1:3]
            detail = " / ".join(NUMBERED_NAME.sub("_", line.strip()) for line in lines)
            return f"panic at {match.group(1)}: {detail[:160]}"
        if self.exit_code < 0:
            return f"signal {-self.exit_code}"
        if self.exit_code >= 128:
            return f"signal {self.exit_code - 128}"
        if "VERIFICATION FAILED" in self.output or "ASSERTION FAILED" in self.output:
            line = next(line for line in self.output.splitlines() if "FAILED" in line)
            return f"assertion {line.strip()[:120]}"
        return None


def run_program(js: Path, path: Path, mode: Mode, seed: int, timeout: float) -> Run:
    wait_until_machine_is_free()
    environment = dict(os.environ)
    environment.pop("LIBJS_JIT", None)
    if mode.jit:
        environment["LIBJS_JIT"] = mode.environment_value(seed)
    try:
        process = subprocess.run(
            [str(js), "--expose-jit-testing", str(path)],
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return Run(0, "", True)
    output = ANSI_ESCAPE.sub("", process.stdout.decode("utf-8", errors="replace"))
    return Run(process.returncode, output, False)


def failure_signature(js: Path, path: Path, mode: Mode, seed: int, timeout: float) -> Optional[str]:
    """How the program fails under `mode`: a crash signature, "mismatch" if its output differs from the
    interpreter's, or None if it does not fail (or the interpreter does not finish)."""
    actual = run_program(js, path, mode, seed, timeout)
    crash = actual.crash_signature()
    if crash:
        return f"crash: {crash}"
    expected = run_program(js, path, INTERPRETER, seed, timeout)
    if expected.timed_out or expected.crash_signature():
        return None
    if actual.timed_out:
        return "timeout"
    if (actual.exit_code, actual.output) != (expected.exit_code, expected.output):
        return "mismatch"
    return None


def reduce_lines(lines: List[str], is_interesting: Callable[[List[str]], bool], log=print) -> List[str]:
    """Removes chunks of lines, then single block openers and their closers, while the program stays interesting."""
    chunk = max(1, len(lines) // 2)
    while True:
        index = 0
        removed_any = False
        while index < len(lines):
            candidate = lines[:index] + lines[index + chunk :]
            if candidate and is_interesting(candidate):
                lines = candidate
                removed_any = True
                log(f"  {len(lines)} lines")
            else:
                index += chunk
        if chunk > 1:
            chunk //= 2
        elif not removed_any:
            break

    # Unwrap blocks: drop a line opening a block together with the line closing it.
    index = 0
    while index < len(lines):
        if lines[index].rstrip().endswith("{"):
            closer = matching_closer(lines, index)
            if closer is not None:
                candidate = lines[:index] + lines[index + 1 : closer] + lines[closer + 1 :]
                if is_interesting(candidate):
                    lines = candidate
                    log(f"  {len(lines)} lines")
                    continue
        index += 1
    return lines


def matching_closer(lines: List[str], opener: int) -> Optional[int]:
    """The line closing the block `lines[opener]` opens, by indentation."""
    indent = len(lines[opener]) - len(lines[opener].lstrip())
    for index in range(opener + 1, len(lines)):
        line = lines[index]
        if line.strip() and len(line) - len(line.lstrip()) == indent:
            return index if line.strip().startswith("}") else None
    return None


def reduce_program(
    js: Path, source: str, mode: Mode, seed: int, scratch: Path, timeout: float = 10.0, log=print
) -> tuple:
    """Reduces `source`, which fails under `mode`. Returns the reduced source and its failure signature."""
    scratch.parent.mkdir(parents=True, exist_ok=True)
    scratch.write_text(source)
    signature = failure_signature(js, scratch, mode, seed, timeout)
    if signature is None:
        return source, None

    def is_interesting(lines: List[str]) -> bool:
        scratch.write_text("\n".join(lines) + "\n")
        return failure_signature(js, scratch, mode, seed, timeout) == signature

    lines = reduce_lines(source.splitlines(), is_interesting, log)
    reduced = "\n".join(lines) + "\n"
    scratch.write_text(reduced)
    return reduced, signature


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("program", type=Path)
    parser.add_argument("--mode", required=True, help="the JIT mode it fails in (see modes.py)")
    parser.add_argument("--seed", type=int, default=1, help="seed of the mode")
    parser.add_argument("--build", type=Path, default=SOURCE_DIR / "Build/release")
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("-o", "--output", type=Path, help="where to write the result (default: <program>.reduced.js)")
    arguments = parser.parse_args()

    mode = MODES_BY_NAME[arguments.mode]
    output = arguments.output or arguments.program.with_suffix(".reduced.js")
    reduced, signature = reduce_program(
        arguments.build / "bin/js",
        arguments.program.read_text(),
        mode,
        arguments.seed,
        output,
        arguments.timeout,
    )
    if signature is None:
        print("The program does not fail in that mode.")
        return 1
    print(f"{signature}: {len(reduced.splitlines())} lines in {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
