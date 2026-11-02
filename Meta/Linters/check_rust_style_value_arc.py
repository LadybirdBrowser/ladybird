#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Keeps std's `Arc` away from style values.

A style value is shared through `StyleValueArc`, whose count is the one word before the value, and C++
retains and releases a value through it. A value in a std `Arc`, which keeps two counts before the
value, still type-checks wherever it is made and turned into a raw pointer, and releasing it as a
`StyleValueArc` then corrupts the heap. So no std `Arc` of a `StyleValueData` may be named.

The check is a line-based heuristic, not a parser: it misses an `Arc::new` of a value bound to a
variable, which review has to catch.
"""

import pathlib
import re
import subprocess
import sys

ROOT = "Libraries/LibWeb/Rust/src"

PATTERNS = [
    re.compile(r"\bArc<\s*(crate::css::style_value::)?StyleValueData\s*>"),
    re.compile(r"\bArc::<\s*(crate::css::style_value::)?StyleValueData\s*>"),
    re.compile(r"(?<!StyleValue)\bArc::new\(\s*(crate::css::style_value::)?StyleValueData::"),
    re.compile(r"\b(OwnedArc|ArcRef)<[^>]*StyleValueData\s*>"),
]


def main():
    files = subprocess.run(
        ["git", "ls-files", "--", f"{ROOT}/*.rs"], capture_output=True, text=True, check=True
    ).stdout.split()
    failures = []
    for file in files:
        for number, line in enumerate(pathlib.Path(file).read_text().splitlines(), 1):
            if any(pattern.search(line) for pattern in PATTERNS):
                failures.append(f"{file}:{number}: {line.strip()}")
    for failure in failures:
        print(f"std Arc of a style value, use StyleValueArc: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
