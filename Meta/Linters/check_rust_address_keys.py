#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Keeps bare addresses out of what LibWeb's Rust code remembers.

An address held as an integer outlives what it names without saying so, and the next allocation there then
passes for the value that was freed: an element takes another's style. Hold a `PointerKey` (css/style/pointer_key.rs),
which holds a count of what it names, or a counter of your own. Handing C++ a pointer with `.cast()` stays fine.

ALLOWED lists the casts that stand, by file, each of which holds what it names some other way, or is compared and
gone within a call. A file holding fewer than listed fails too, so that the list only shrinks.

The check is a line-based heuristic, not a parser: review has to catch what it misses.
"""

import pathlib
import re
import subprocess
import sys

ROOTS = ["Libraries/LibWeb/Rust/src/css", "Libraries/LibWeb/Rust/src/layout", "Libraries/LibWeb/Rust/src/painting"]

PATTERN = re.compile(
    r"as_ptr\([^()]*\)\s+as usize"
    r"|\.pointer\(\)\s+as usize"
    r"|\.addr\(\)"
    r"|from_ref\([^()]*\)\s+as usize"
    r"|expose_provenance\(\)"
)

ALLOWED = {
    "css/computed_values.rs": 1,
    "css/custom_properties.rs": 2,
    "css/parser/syntax_parser/ownership_tests.rs": 2,
    "css/rule/read.rs": 1,
    "css/style/bridge.rs": 1,
    "css/style/custom_property_environments.rs": 1,
    "css/style/effect_descriptions.rs": 1,
    "css/style/engine_calls.rs": 1,
    "css/style/font_resolution.rs": 2,
    "css/style/kept_past_release.rs": 3,
    "css/style/pipeline/compute.rs": 1,
    "css/style/pipeline/custom_properties.rs": 4,
    "css/style/pipeline/custom_property_moves.rs": 1,
    "css/style/pipeline/install.rs": 1,
    "css/style/pipeline/records.rs": 2,
    "css/style/pipeline/rules.rs": 2,
    "css/style/pipeline/sharing.rs": 1,
    "css/style/pipeline/traversal.rs": 3,
    "css/style_compute/ffi_test_stubs.rs": 1,
    "layout/layout_node_arena.rs": 1,
}


def main():
    files = subprocess.run(
        ["git", "ls-files", "--", *(f"{root}/*.rs" for root in ROOTS)], capture_output=True, text=True, check=True
    ).stdout.split()
    failures = []
    for file in files:
        if file.endswith("/pointer_key.rs"):
            continue
        lines = [
            (number, line)
            for number, line in enumerate(pathlib.Path(file).read_text().splitlines(), 1)
            if PATTERN.search(line)
        ]
        allowed = ALLOWED.get(file.removeprefix("Libraries/LibWeb/Rust/src/"), 0)
        if len(lines) > allowed:
            failures.extend(f"{file}:{number}: {line.strip()}" for number, line in lines)
        elif len(lines) < allowed:
            failures.append(f"{file}: holds {len(lines)} address casts, lower its allowance of {allowed}")
    for failure in failures:
        print(f"bare address, hold a PointerKey: {failure}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
