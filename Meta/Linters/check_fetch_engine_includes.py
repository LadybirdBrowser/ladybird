#!/usr/bin/env python3

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
ENGINE_ROOT = pathlib.Path("Libraries/LibWeb/Fetch/Engine")
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
FORBIDDEN_PREFIXES = ("LibJS/", "LibGC/", "LibWeb/")
ALLOWED_PREFIXES = ("LibWeb/Fetch/Engine/", "LibWeb/Export.h")

CHECKED_SUFFIXES = {".cpp", ".h"}


def is_forbidden(included_path: str) -> bool:
    return included_path.startswith(FORBIDDEN_PREFIXES) and not included_path.startswith(ALLOWED_PREFIXES)


def main() -> int:
    offenders = []
    for path in sorted((REPO_ROOT / ENGINE_ROOT).rglob("*")):
        if path.suffix not in CHECKED_SUFFIXES:
            continue
        for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            match = INCLUDE.match(line)
            if match and is_forbidden(match.group(1)):
                offenders.append(f"{path.relative_to(REPO_ROOT)}:{line_number}: {line.strip()}")

    if not offenders:
        return 0

    print(
        f"{ENGINE_ROOT} must not include LibJS, LibGC, or LibWeb outside {ENGINE_ROOT} and LibWeb/Export.h:",
        file=sys.stderr,
    )
    for offender in offenders:
        print(f"  {offender}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
