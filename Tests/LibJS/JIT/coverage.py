#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Reports which IR ops, speculations and exits the JIT's tests reach.

Runs with LIBJS_JIT=...,coverage=DIRECTORY write what the code they compiled contained (one key per node, like
"Int32Binary.Add" or "LoadElementAt.TypedArray(Int8)", and per place it can exit, like "exit-site.BadShape") and what
happened to it ("exit.Overflow", "stress-exit.BadShape", "osr-entry", "discard", ...) into that directory. This adds
them up and compares them with every op and kind there is, read from the compiler's source, so that new ops and exit
kinds show up as not covered until a test reaches them.

Usage: coverage.py DIRECTORY... [--json report.json] [--baseline report.json]
With --baseline, fails if a key the baseline covered is not covered anymore.
"""

import argparse
import json
import re
import sys

from collections import Counter
from pathlib import Path
from typing import Dict
from typing import List

SOURCE_DIR = Path(__file__).resolve().parents[3]
JIT_SOURCE = SOURCE_DIR / "Libraries/LibJS/JIT/Rust/src"

# Keys that cannot be covered, with the reason.
IMPOSSIBLE = {
    "Int32Binary.Div": "Div is not an int32 operation",
    "Constant": "constants are in no block",
    "VirtualObject": "virtual objects are in no block",
    "exit.Leave": "leaves are not exits",
    "exit.LeaveMaterialized": "leaves are not exits",
    "stress-exit.Leave": "leaves are not exits",
    "stress-exit.LeaveMaterialized": "leaves are not exits",
    "exit-site.Leave": "leaves are not exits",
    "exit-site.LeaveMaterialized": "leaves are not exits",
    "stress-exit.NoFeedback": "unconditional exits are not forced",
    "exit.InlinedCall": "calls in inlined code are not exits",
    "exit-site.InlinedCall": "calls in inlined code are not exits",
    "stress-exit.InlinedCall": "calls in inlined code are not exits",
}


def enum_variants(path: Path, name: str) -> List[str]:
    """The variants of `pub enum name` in a Rust source file."""
    text = path.read_text()
    match = re.search(rf"pub enum {name} \{{\n(.*?)\n\}}", text, re.DOTALL)
    if not match:
        raise ValueError(f"No enum {name} in {path}")
    return re.findall(r"^    ([A-Z][A-Za-z0-9]*)\b", match.group(1), re.MULTILINE)


def universe() -> Dict[str, List[str]]:
    """Every coverage key there is, by category."""
    ops = enum_variants(JIT_SOURCE / "ir/mod.rs", "Op")
    exit_kinds = enum_variants(JIT_SOURCE / "code.rs", "ExitKind")
    binary_ops = enum_variants(JIT_SOURCE / "ir/mod.rs", "BinaryOp")
    comparisons = enum_variants(JIT_SOURCE / "ir/mod.rs", "Comparison")
    typed_array_elements = enum_variants(JIT_SOURCE / "ir/mod.rs", "TypedArrayElement")
    elements_kinds = ["Packed", "Holey"] + [f"TypedArray({element})" for element in typed_array_elements]

    refined = {
        "Int32Binary": binary_ops,
        "Int32Compare": comparisons,
        "CheckElements": elements_kinds,
        "LoadElementAt": elements_kinds,
        "StoreElementAt": elements_kinds,
    }
    # NB: Other ops come in kinds too (Generic and CallSlowPath by opcode, Exit by exit kind, ...), but not all of
    #     their kinds are expected to be compiled, so only the op is.
    op_keys = []
    for op in ops:
        op_keys += [f"{op}.{kind}" for kind in refined[op]] if op in refined else [op]
    return {
        "ops": op_keys,
        "exit sites": [f"exit-site.{kind}" for kind in exit_kinds],
        "exits taken": [f"exit.{kind}" for kind in exit_kinds],
        "stress exits taken": [f"stress-exit.{kind}" for kind in exit_kinds],
        "branches": ["Branch.Bool", "Branch.Nullish", "Branch.Undefined", "Branch.TaggedEquals"]
        + [f"Branch.Int32.{comparison}" for comparison in comparisons],
        "events": [
            "compile",
            "discard",
            "refuse",
            "osr-entry",
            "stress-invalidate",
            "exit.from-inlined-code",
            "graph.loop",
            "graph.osr-entry",
        ]
        + [f"graph.inlined-depth.{depth}" for depth in range(1, 6)],
    }


def covered_keys(counts: Counter) -> Counter:
    """The counts, with the counts of refined keys also counted for their unrefined op (Generic.Call counts for
    Generic), so that ops covered in any kind count as covered."""
    result = Counter(counts)
    for key, count in counts.items():
        if "." in key and not key.startswith(("exit", "stress-exit", "graph", "compile-failure")):
            result[key.split(".", 1)[0]] += count
    return result


def load(directories: List[Path]) -> Counter:
    counts: Counter = Counter()
    for directory in directories:
        for path in directory.rglob("coverage-*.json"):
            try:
                counts.update(json.loads(path.read_text()))
            except json.JSONDecodeError:
                print(f"Skipping unreadable {path}", file=sys.stderr)
    return counts


def report(counts: Counter) -> dict:
    counts = covered_keys(counts)
    categories = {}
    for category, keys in universe().items():
        possible = [key for key in keys if key not in IMPOSSIBLE]
        covered = [key for key in possible if counts.get(key)]
        categories[category] = {
            "covered": len(covered),
            "total": len(possible),
            "missing": [key for key in possible if not counts.get(key)],
        }
    others = sorted(key for key in counts if key.startswith(("Generic.", "CallSlowPath.", "compile-failure.")))
    return {
        "categories": categories,
        "generic and slow path opcodes": {key: counts[key] for key in others},
        "counts": dict(sorted(counts.items())),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--json", type=Path, help="write the report as JSON")
    parser.add_argument("--baseline", type=Path, help="a report whose covered keys must stay covered")
    parser.add_argument("-v", "--verbose", action="store_true", help="list the counts of every key")
    arguments = parser.parse_args()

    counts = load(arguments.directories)
    if not counts:
        print("No coverage files found")
        return 1
    result = report(counts)
    for category, data in result["categories"].items():
        percent = 100 * data["covered"] / max(data["total"], 1)
        print(f"{category}: {data['covered']}/{data['total']} ({percent:.0f}%)")
        if data["missing"]:
            print(f"  not covered: {', '.join(data['missing'])}")
    generic = [key for key in result["generic and slow path opcodes"] if key.startswith("Generic.")]
    print(f"opcodes compiled generically: {len(generic)}")
    if arguments.verbose:
        for key, count in result["counts"].items():
            print(f"  {key}: {count}")
    if arguments.json:
        arguments.json.write_text(json.dumps(result, indent=2) + "\n")

    if arguments.baseline:
        baseline = json.loads(arguments.baseline.read_text())
        lost = [key for key, count in baseline["counts"].items() if count and not result["counts"].get(key)]
        if lost:
            print(f"No longer covered: {', '.join(lost)}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
