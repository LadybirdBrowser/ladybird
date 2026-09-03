#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Summarize inclusive stack weights from an exported xctrace Time Profiler table."""

import argparse
import json
import re
import xml.etree.ElementTree as ET

from collections import defaultdict


def _resolve_backtrace(backtrace, frames, backtraces):
    if reference := backtrace.get("ref"):
        return backtraces[reference]

    names = []
    for frame in backtrace.findall("frame"):
        if reference := frame.get("ref"):
            name = frames[reference]
        else:
            name = frame.get("name", "")
            if identity := frame.get("id"):
                frames[identity] = name
        names.append(name)
    if identity := backtrace.get("id"):
        backtraces[identity] = names
    return names


def inclusive_weights(path):
    """Return (sample count, {frame name: inclusive nanoseconds})."""

    frames = {}
    backtraces = {}
    tagged_backtraces = {}
    weights = {}
    inclusive = defaultdict(int)
    sample_count = 0

    for _, row in ET.iterparse(path, events=("end",)):
        if row.tag != "row":
            continue

        weight = 0
        names = []
        for column in row:
            if column.tag == "weight":
                if identity := column.get("id"):
                    weights[identity] = int(column.text or 0)
                if reference := column.get("ref"):
                    weight = weights[reference]
                else:
                    weight = int(column.text or 0)
            elif column.tag == "tagged-backtrace":
                if reference := column.get("ref"):
                    names = tagged_backtraces[reference]
                    continue
                # xctrace 16.0 (Xcode 26) nests a backtrace element inside each tagged-backtrace, and xctrace 27.0
                # (Xcode 27) puts the frame elements directly under it — so resolve whichever element holds the frames.
                backtrace = column.find("backtrace")
                names = _resolve_backtrace(column if backtrace is None else backtrace, frames, backtraces)
                if identity := column.get("id"):
                    tagged_backtraces[identity] = names

        sample_count += 1
        for name in set(names):
            inclusive[name] += weight
        row.clear()

    return sample_count, dict(inclusive)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("export", help="XML exported from xctrace's time-profile table")
    parser.add_argument("--contains", action="append", default=[], help="include frames containing this text")
    parser.add_argument("--regex", help="include frames matching this regular expression")
    parser.add_argument("--limit", type=int, default=40, help="maximum frames to print (default: 40)")
    parser.add_argument("--minimum-ms", type=float, default=0.0, help="omit frames below this inclusive weight")
    parser.add_argument("--json", action="store_true", help="write machine-readable JSON")
    args = parser.parse_args()

    sample_count, weights = inclusive_weights(args.export)
    expression = re.compile(args.regex) if args.regex else None
    selected = [
        (name, nanoseconds)
        for name, nanoseconds in weights.items()
        if nanoseconds >= args.minimum_ms * 1_000_000
        and (not args.contains or any(text in name for text in args.contains))
        and (expression is None or expression.search(name))
    ]
    selected.sort(key=lambda item: (-item[1], item[0]))
    selected = selected[: args.limit]

    if args.json:
        print(
            json.dumps(
                {
                    "samples": sample_count,
                    "frames": [
                        {"name": name, "inclusiveMilliseconds": nanoseconds / 1_000_000}
                        for name, nanoseconds in selected
                    ],
                },
                indent=2,
            )
        )
        return

    print(f"Samples: {sample_count}")
    print("Inclusive (ms)  Frame")
    for name, nanoseconds in selected:
        print(f"{nanoseconds / 1_000_000:14.1f}  {name}")


if __name__ == "__main__":
    main()
