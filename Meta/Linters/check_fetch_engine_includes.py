#!/usr/bin/env python3

import collections
import pathlib
import re
import sys

from typing import Optional

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
THIS_SCRIPT = pathlib.Path(__file__).resolve().relative_to(REPO_ROOT)
LIBRARIES_ROOT = pathlib.Path("Libraries")
ENGINE_ROOT = LIBRARIES_ROOT / "LibWeb/Fetch/Engine"
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')

# The source tree directories whose headers the check follows.
FOLLOWED_ROOTS = {"AK", "Libraries"}
FORBIDDEN_PREFIXES = ("LibJS/", "LibGC/")

# Defines the macros that declare a type holds no cell edges, and the trait they specialize.
ALLOWED_GC_HEADER = "LibGC/HoldsNoCellEdges.h"

# The generated headers the engine reaches, which hold debug macros, export macros, a bindings enum, and IPC codec and
# Rust FFI declarations.
GENERATED_HEADERS = {
    "AK/Debug.h",
    "LibCore/Export.h",
    "LibHTTP/Cookie/CookieCodecs.h",
    "LibURL/RustFFI.h",
    "LibWeb/Export.h",
    "LibWebCommon/Bindings/SecurityPolicyViolationEvent.h",
    "LibWebCommon/Export.h",
    "LibWebCommon/MixedContent/ProhibitsMixedSecurityContextsCodecs.h",
    "LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicyCodecs.h",
}

CHECKED_SUFFIXES = {".cpp", ".h"}


def includes_of(path):
    for line_number, line in enumerate((REPO_ROOT / path).read_text(encoding="utf-8").splitlines(), start=1):
        match = INCLUDE.match(line)
        if match:
            yield line_number, line.strip(), match.group(1) == '"', match.group(2)


def resolve(path, is_quoted, included_path):
    candidates = [(REPO_ROOT / path).parent / included_path] if is_quoted else []
    candidates.append(REPO_ROOT / LIBRARIES_ROOT / included_path)
    candidates.append(REPO_ROOT / included_path)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve().relative_to(REPO_ROOT)
    return None


def names_a_source_tree_library(included_path):
    first_component = pathlib.PurePosixPath(included_path).parts[0]
    return (REPO_ROOT / LIBRARIES_ROOT / first_component).is_dir() or (REPO_ROOT / first_component).is_dir()


def main() -> int:
    engine_files = [
        path.relative_to(REPO_ROOT)
        for path in sorted((REPO_ROOT / ENGINE_ROOT).rglob("*"))
        if path.suffix in CHECKED_SUFFIXES
    ]

    reached_by: dict[pathlib.Path, Optional[tuple[pathlib.Path, int, str]]] = dict.fromkeys(engine_files)
    queue = collections.deque(engine_files)
    forbidden = []
    unresolved = []
    reached_generated_headers = set()

    while queue:
        path = queue.popleft()
        for line_number, line, is_quoted, included_path in includes_of(path):
            include = (path, line_number, line)
            resolved = resolve(path, is_quoted, included_path)
            if resolved is not None and resolved.is_relative_to(LIBRARIES_ROOT):
                included_path = resolved.relative_to(LIBRARIES_ROOT).as_posix()

            if included_path.startswith(FORBIDDEN_PREFIXES) and included_path != ALLOWED_GC_HEADER:
                forbidden.append((included_path, include))
            elif included_path in GENERATED_HEADERS:
                reached_generated_headers.add(included_path)
            elif resolved is None:
                if names_a_source_tree_library(included_path):
                    unresolved.append((included_path, include))
            elif resolved.parts[0] in FOLLOWED_ROOTS and resolved not in reached_by:
                reached_by[resolved] = include
                queue.append(resolved)

    def print_chains(offenders):
        for included_path, include in offenders:
            chain = [include]
            while reached_by[chain[-1][0]] is not None:
                chain.append(reached_by[chain[-1][0]])
            print(f"  {included_path}, through:", file=sys.stderr)
            for path, line_number, line in reversed(chain):
                print(f"    {path}:{line_number}: {line}", file=sys.stderr)

    if forbidden:
        print(
            f"{ENGINE_ROOT} must not include LibJS or LibGC, directly or through other headers, except "
            f"{ALLOWED_GC_HEADER}:",
            file=sys.stderr,
        )
        print_chains(forbidden)
    if unresolved:
        print(
            f"{ENGINE_ROOT} must not include a library header that is not in the source tree, such as a generated one "
            f"outside GENERATED_HEADERS in {THIS_SCRIPT}:",
            file=sys.stderr,
        )
        print_chains(unresolved)

    unreached_generated_headers = sorted(GENERATED_HEADERS - reached_generated_headers)
    for header in unreached_generated_headers:
        print(
            f"{header} is no longer reached from {ENGINE_ROOT}; remove it from GENERATED_HEADERS in {THIS_SCRIPT}.",
            file=sys.stderr,
        )

    return 1 if forbidden or unresolved or unreached_generated_headers else 0


if __name__ == "__main__":
    sys.exit(main())
