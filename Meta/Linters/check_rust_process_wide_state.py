#!/usr/bin/env python3
"""Keeps process-wide state out of the Rust crates that are compiled into more than one library.

A Rust crate that another Rust crate depends on is compiled into both of their libraries, and so
is every `static` in it. A dynamic linker with a flat namespace binds most calls to one of the
copies and the duplication never shows; one with two-level namespaces, as macOS has, gives each
library its own copy of the state, and a library that hides its symbols has its own copy
everywhere. A counter one library increments is then not the counter the other reads, and nothing
about the Rust says so.

So state that has to be shared lives on the C++ side, in a library there is only one of, and the
crate reaches it through `extern "C"` accessors (see Libraries/LibGfx/RustProcessState.cpp).
Everything that stays in such a crate has to be listed below with the reason it may be per copy.

The check is a line-based heuristic, not a parser: it misses a static whose type starts on the next
line or hides its interior mutability behind a type alias.
"""

import pathlib
import re
import subprocess
import sys

# `file:name` -> why a per-copy instance is harmless: a cache of a pure function, per-thread scratch,
# or a stub in a test binary.
ALLOWED = {
    "Libraries/LibGfx/Rust/src/text_layout.rs:SHAPING_CACHE": "per-thread memo of a pure function; a second copy costs memory, never an answer",
    "Libraries/LibCompositing/Rust/src/test_stubs.rs:NEXT": "test stub for LibGfx's C++ counters, in the cargo test binary that has no C++ side",
    "Libraries/LibCompositing/Rust/src/display_list/replay.rs:WARM_REPLAY_SCRATCH_STORAGE": "per-thread replay scratch; a second copy costs memory, never an answer",
    "Libraries/LibJS/Flap/src/low_ir/lowering.rs:LABELS": "per-thread scratch buffer; a second copy costs memory, never an answer",
    "Libraries/LibWeb/HTML/Parser/Rust/src/token.rs:SPARE_ATTRIBUTE_LISTS": "per-thread allocation pool; a second copy costs memory, never an answer",
    "Libraries/LibWeb/HTML/Parser/Rust/src/token.rs:SPARE_ATTRIBUTE_VALUES": "per-thread allocation pool; a second copy costs memory, never an answer",
}

STATE = re.compile(r"^\s*(?:pub(?:\([^)]*\))?\s+)?static\s+(mut\s+)?([A-Z_][A-Z0-9_]*)\s*:\s*(.*)$")

# A `static` whose type has none of these, and that is not `static mut`, cannot be written, so it is
# constant data, not state. A `thread_local!` value is only writable through one of these too.
INTERIOR_MUTABILITY = re.compile(r"Cell|Atomic|Mutex|RwLock|Once|Lazy|Condvar")


def tracked_files(root, pattern):
    output = subprocess.run(["git", "ls-files", "--", pattern], cwd=root, capture_output=True, text=True, check=True)
    return [root / line for line in output.stdout.splitlines()]


def crate_name(manifest):
    match = re.search(r"^name\s*=\s*\"([^\"]+)\"", manifest.read_text(), re.M)
    return match.group(1) if match else None


def crates_compiled_into_more_than_one_library(manifests):
    """Every crate some other crate depends on by path: its code, and its statics, end up in both."""
    shared = set()
    for manifest in manifests:
        for match in re.finditer(r"^([a-z_0-9]+)\s*=\s*\{[^}]*\bpath\s*=", manifest.read_text(), re.M):
            shared.add(match.group(1))
    return shared


def state_in(root, source_directory):
    """Yields `(relative path, line number, name)` for every writable static in the crate's sources."""
    for source in sorted(source_directory.glob("**/*.rs")):
        relative = source.relative_to(root).as_posix()
        for number, line in enumerate(source.read_text().splitlines(), start=1):
            match = STATE.match(line)
            if match and (match.group(1) or INTERIOR_MUTABILITY.search(match.group(3))):
                yield relative, number, match.group(2)


def main():
    root = pathlib.Path(__file__).resolve().parent.parent.parent
    manifests = tracked_files(root, "*Cargo.toml")
    shared_crates = crates_compiled_into_more_than_one_library(manifests)
    failures = []
    present = set()
    for manifest in manifests:
        if crate_name(manifest) not in shared_crates:
            continue
        for relative, number, name in state_in(root, manifest.parent / "src"):
            present.add(f"{relative}:{name}")
            if f"{relative}:{name}" not in ALLOWED:
                failures.append(f"{relative}:{number}: {name}")
    # An entry whose state has gone must go too, or it would let the state come back unnoticed.
    for entry in sorted(ALLOWED.keys() - present):
        failures.append(f"{entry}: listed in ALLOWED but no longer present")

    if not failures:
        return 0
    print("A Rust crate compiled into more than one library must keep no process-wide state:")
    for failure in failures:
        print(f"  {failure}")
    print("Move it behind C++-owned storage (see Libraries/LibGfx/RustProcessState.cpp), or add it")
    print("to ALLOWED in this script with the reason a per-copy instance is harmless.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
