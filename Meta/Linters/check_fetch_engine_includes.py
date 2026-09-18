#!/usr/bin/env python3

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ENGINE_ROOT = pathlib.Path("Libraries/LibWeb/Fetch/Engine")
FORBIDDEN_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](LibJS|LibGC)/')

CHECKED_SUFFIXES = {".cpp", ".h"}


def main() -> int:
    offenders = []
    for path in sorted((REPO_ROOT / ENGINE_ROOT).rglob("*")):
        if path.suffix not in CHECKED_SUFFIXES:
            continue
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            if FORBIDDEN_INCLUDE.match(line):
                offenders.append(f"{path.relative_to(REPO_ROOT)}:{line_number}: {line.strip()}")

    if not offenders:
        return 0

    print(f"{ENGINE_ROOT} must not include LibJS or LibGC:", file=sys.stderr)
    for offender in offenders:
        print(f"  {offender}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
