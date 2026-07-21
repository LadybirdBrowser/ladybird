#!/usr/bin/env python3
"""Runs the style FFI baseline workloads and prints the per-workload FFI
boundary counters (see internals.styleFfiCounters()).

The workloads are deterministic pages that build a DOM shape, force a style
update, and print every counter. They are copied into the LibWeb test tree
for the run so test-web can drive them, then removed again; the printed
counts are a measurement of the current boundary, not a regression test.

Usage: Meta/StyleFfiBaseline/collect.py [build-dir]
"""

import pathlib
import shutil
import subprocess
import sys

script_dir = pathlib.Path(__file__).resolve().parent
repo_root = script_dir.parent.parent
build_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else repo_root / "Build" / "release"

input_dir = repo_root / "Tests" / "LibWeb" / "Text" / "input" / "css" / "ffi-baseline"
expected_dir = repo_root / "Tests" / "LibWeb" / "Text" / "expected" / "css" / "ffi-baseline"

try:
    input_dir.mkdir(parents=True)
    for page in sorted(script_dir.glob("*.html")):
        shutil.copy(page, input_dir)
    shutil.copy(script_dir / "harness.js", input_dir)

    subprocess.run(
        ["./bin/test-web", "--rebaseline", "-f", "Text/input/css/ffi-baseline"],
        cwd=build_dir,
        check=True,
        stdout=subprocess.DEVNULL,
    )

    for expected in sorted(expected_dir.glob("*.txt")):
        print(f"=== {expected.stem}")
        print(expected.read_text(), end="")
finally:
    shutil.rmtree(input_dir, ignore_errors=True)
    shutil.rmtree(expected_dir, ignore_errors=True)
