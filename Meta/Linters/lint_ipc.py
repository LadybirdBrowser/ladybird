#!/usr/bin/env python3

# Copyright (c) 2021, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import sys

from collections import defaultdict
from pathlib import Path
from typing import Dict
from typing import List

sys.path.append(str(Path(__file__).resolve().parent.parent.parent))

from Meta.Generators.string_hash import string_hash

ENDPOINT_PREFIX = "endpoint "


def main() -> int:
    parser = argparse.ArgumentParser(description="Check IPC endpoint files for magic-number collisions.")
    parser.add_argument("ipc_files", nargs="+", help="IPC endpoint definition files")
    args = parser.parse_args()

    files_by_magic: Dict[int, List[str]] = defaultdict(list)
    error_count = 0

    def report_error(message: str) -> None:
        nonlocal error_count
        print(f"Error: {message}", file=sys.stderr)
        error_count += 1

    for path in args.ipc_files:
        endpoint_name = ""

        try:
            with open(path, "r", encoding="utf-8") as ipc_file:
                for line in ipc_file:
                    line = line.strip()
                    if not line.startswith(ENDPOINT_PREFIX):
                        continue

                    remaining = line[len(ENDPOINT_PREFIX) :]
                    if endpoint_name:
                        report_error(f"Multiple endpoints in file '{path}': Found {endpoint_name} and {remaining}")
                        continue

                    endpoint_name = remaining
        except OSError as error:
            report_error(f"Cannot open '{path}': {error}")
            continue

        if not endpoint_name:
            report_error(f"Could not detect endpoint name in file '{path}'")
            continue

        files_by_magic[string_hash(endpoint_name)].append(path)

    for magic, files in files_by_magic.items():
        if len(files) <= 1:
            continue

        report_error(f"Collision: Multiple endpoints use the magic number {magic}:")
        for colliding_file in files:
            print(f"  - {colliding_file}")

    print(f"Checked {len(args.ipc_files)} files, saw {len(files_by_magic)} distinct magic numbers.")

    if error_count:
        print(
            "Some errors were encountered. There may be endpoints with colliding magic numbers.",
            file=sys.stderr,
        )

    return error_count


if __name__ == "__main__":
    sys.exit(main())
