#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Differential fuzzing of the JIT: random programs (generator.py) run in the interpreter and under JIT stress modes,
and every crash or difference in their output is a bug.

Each program runs in the interpreter and in a few of the modes, chosen by its number. Failures are saved in the output
directory with a command that reproduces them, reduced (reduce.py) and deduplicated by their crash signature or, for
output differences, by their reduced program. findings.md there lists one line per distinct failure.

The campaign pauses while the machine is needed idle (see machine.py). Program N of seed S is generator.py's program
S * 1000000 + N, so every failure reproduces from its number.
"""

import argparse
import concurrent.futures
import hashlib
import json
import re
import sys
import threading
import time

from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from generator import generate  # noqa: E402
from machine import wait_until_machine_is_free  # noqa: E402
from modes import INTERPRETER  # noqa: E402
from modes import MODES_BY_NAME  # noqa: E402
from reduce import reduce_program  # noqa: E402
from reduce import run_program  # noqa: E402

SOURCE_DIR = Path(__file__).resolve().parents[3]
FUZZ_MODES = [
    "warm",
    "exits",
    "eager",
    "osr",
    "registers",
    "deep-inline",
    "random",
    "exits-always",
    "no-inline",
    "async",
    "invalidate",
]
IDENTIFIER = re.compile(r"\b(v|p|f)[0-9_]+\b")


class Campaign:
    def __init__(self, arguments):
        self.arguments = arguments
        self.js = arguments.build / "bin/js"
        self.out = arguments.out
        self.lock = threading.Lock()
        self.signatures = {}
        self.counts = {"programs": 0, "runs": 0, "skipped": 0, "failures": 0, "distinct": 0}
        self.started = time.monotonic()
        findings = self.out / "findings.json"
        if findings.exists():
            self.signatures = json.loads(findings.read_text())
            self.counts["distinct"] = len(self.signatures)

    def modes_for(self, number: int):
        modes = self.arguments.modes
        count = min(self.arguments.modes_per_program, len(modes))
        return [MODES_BY_NAME[modes[(number * count + k) % len(modes)]] for k in range(count)]

    def run_one(self, number: int) -> None:
        wait_until_machine_is_free()
        seed = self.arguments.seed * 1000000 + number
        source = generate(seed)
        work = self.out / "work" / f"{seed}.js"
        work.parent.mkdir(parents=True, exist_ok=True)
        work.write_text(source)
        expected = run_program(self.js, work, INTERPRETER, seed, self.arguments.timeout)
        with self.lock:
            self.counts["programs"] += 1
            self.counts["runs"] += 1
        if expected.timed_out or expected.crash_signature():
            with self.lock:
                self.counts["skipped"] += 1
            work.unlink()
            return
        for mode in self.modes_for(number):
            actual = run_program(self.js, work, mode, seed, self.arguments.timeout * 3)
            with self.lock:
                self.counts["runs"] += 1
            crash = actual.crash_signature()
            if crash:
                self.record_failure(seed, mode, source, f"crash: {crash}", actual.output)
            elif actual.timed_out:
                self.record_failure(seed, mode, source, "timeout", "")
            elif (actual.exit_code, actual.output) != (expected.exit_code, expected.output):
                self.record_failure(seed, mode, source, "mismatch", actual.output)
        work.unlink()

    def record_failure(self, seed: int, mode, source: str, signature: str, output: str) -> None:
        with self.lock:
            self.counts["failures"] += 1
            known = signature.startswith("crash") and signature in self.signatures
        directory = self.out / "failures" / f"{seed}-{mode.name}"
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "program.js").write_text(source)
        # NB: Panics come first and the verifiers' dumps after them.
        (directory / "output.txt").write_text(
            output if len(output) < 40000 else output[:20000] + "\n...\n" + output[-20000:]
        )
        repro = (
            f"python3 {Path(__file__).parent / 'generator.py'} {seed} > p.js && "
            f"LIBJS_JIT={mode.environment_value(seed)} {self.js} --expose-jit-testing p.js"
        )
        (directory / "repro.txt").write_text(repro + "\n")
        if known:
            return
        reduced, reduced_signature = source, signature
        if self.arguments.reduce:
            reduced, reduced_signature = reduce_program(
                self.js, source, mode, seed, directory / "reduced.js", self.arguments.timeout, log=lambda _: None
            )
            if reduced_signature is None:
                reduced_signature = signature
        key = reduced_signature
        if not key.startswith("crash"):
            key = f"{reduced_signature}: {hashlib.sha1(IDENTIFIER.sub('x', reduced).encode()).hexdigest()[:12]}"
        with self.lock:
            if key in self.signatures:
                return
            self.signatures[key] = {
                "seed": seed,
                "mode": mode.name,
                "jit": mode.environment_value(seed),
                "directory": str(directory),
                "lines": len(reduced.splitlines()),
                "found": time.strftime("%Y-%m-%d %H:%M:%S"),
            }
            self.counts["distinct"] = len(self.signatures)
            (self.out / "findings.json").write_text(json.dumps(self.signatures, indent=2))
            with open(self.out / "findings.md", "a") as findings:
                findings.write(
                    f"- {key}: seed {seed}, mode {mode.name}, {len(reduced.splitlines())} lines, {directory}\n"
                )
            print(f"NEW {key} (seed {seed}, mode {mode.name}) in {directory}", flush=True)

    def report(self) -> None:
        elapsed = time.monotonic() - self.started
        with self.lock:
            counts = dict(self.counts)
        print(
            f"[{elapsed / 60:.1f} min] {counts['programs']} programs ({counts['programs'] / max(elapsed, 1):.1f}/s), "
            f"{counts['runs']} runs, {counts['skipped']} skipped, {counts['failures']} failures, "
            f"{counts['distinct']} distinct",
            flush=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", type=Path, default=SOURCE_DIR / "Build/release")
    parser.add_argument("--seed", type=int, default=1, help="campaign seed")
    parser.add_argument("--start", type=int, default=0, help="number of the first program")
    parser.add_argument("--count", type=int, default=0, help="programs to run (default: until --seconds)")
    parser.add_argument("--seconds", type=int, default=0, help="how long to run (default: until --count)")
    parser.add_argument("-j", "--jobs", type=int, default=4)
    parser.add_argument("--modes", default=",".join(FUZZ_MODES), help="modes to run programs in")
    parser.add_argument("--modes-per-program", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=5.0, help="seconds for the interpreter run of a program")
    parser.add_argument("--no-reduce", dest="reduce", action="store_false", help="keep failures unreduced")
    parser.add_argument("--out", type=Path, default=Path("/tmp/jit-fuzz"))
    arguments = parser.parse_args()
    arguments.modes = arguments.modes.split(",")
    arguments.out.mkdir(parents=True, exist_ok=True)

    campaign = Campaign(arguments)
    deadline = time.monotonic() + arguments.seconds if arguments.seconds else None
    number = arguments.start
    last_report = time.monotonic()
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as executor:
        pending = set()
        while True:
            done = deadline is not None and time.monotonic() > deadline
            done = done or (arguments.count and number >= arguments.start + arguments.count)
            while not done and len(pending) < arguments.jobs * 2:
                pending.add(executor.submit(campaign.run_one, number))
                number += 1
                done = arguments.count and number >= arguments.start + arguments.count
            if not pending:
                break
            finished, pending = concurrent.futures.wait(pending, timeout=5, return_when="FIRST_COMPLETED")
            for future in finished:
                future.result()
            if time.monotonic() - last_report > 60:
                campaign.report()
                last_report = time.monotonic()
            if done and not pending:
                break
    campaign.report()
    return 1 if campaign.signatures else 0


if __name__ == "__main__":
    sys.exit(main())
