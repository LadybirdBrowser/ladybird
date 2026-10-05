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

It also keeps count of the state in libweb_rust, whose render states are to move to a thread that
renders. A thread other than the document's either shares a `static` with it or silently gets a
fresh `thread_local!` of its own, and either can be wrong, so every one there has to be listed
below with the reason it is fine on another thread.

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

# libweb_rust: `file:name` -> why the state is fine on a thread other than the document's.
RENDER_STATE_CRATE = "Libraries/LibWeb/Rust"

STYLE_POOL = "style interning pool behind a mutex; becomes per-engine state before style runs on another thread"
SCRATCH = "per-thread allocation pool; a thread without one only allocates more"
DIAGNOSTIC = "counter diagnostics; off unless an environment variable turns them on"
ENVIRONMENT_SWITCH = "read-once environment switch; every thread sees the same answer"
IDENTITY = "process-wide atomic counter handing out unique identities"
BUILT_ONCE = "built once and read-only after; every thread shares the same table"
LOCKED = "process-wide and behind a mutex or a lock"
TEST_ONLY = "test only"


def render_state_entries(reason, entries):
    return {f"{RENDER_STATE_CRATE}/src/{entry}": reason for entry in entries}


RENDER_STATE_ALLOWED = {
    **render_state_entries(
        STYLE_POOL,
        [
            "css/style/matching.rs:DISPATCH_POOLS",
            "css/style/native_rules/targets.rs:TARGET_PAGES",
            "css/style/prefix/relation.rs:RELATION_PROGRAM_MEMORY",
            "css/style/program.rs:RULE_DECLARATIONS",
            "css/style/program/rule_records.rs:RULE_RECORD_PAGES",
            "css/style/program/rule_versions.rs:RULE_VERSION_PAGES",
            "css/style/selector.rs:ROUTING_POOLS",
            "css/style/selector.rs:SELECTOR_PROGRAM_POOLS",
        ],
    ),
    **render_state_entries(
        SCRATCH,
        [
            "css/cascaded_properties.rs:STORE_POOL",
            "css/style/column.rs:STAMPED_INDEX_POOL",
        ],
    ),
    **render_state_entries(
        DIAGNOSTIC,
        [
            "css/ffi_stats.rs:COMPLETE_STYLE_UPDATE_STATE",
            "css/ffi_stats.rs:COUNTERS_ENABLED",
            "css/ffi_stats.rs:COUNTER_CONTEXT",
            "css/ffi_stats.rs:CPP_CALLBACK_COUNT",
            "css/ffi_stats.rs:REGISTRY",
            "css/ffi_stats.rs:THREAD_UNSAFE_CPP_CALLBACK_COUNT",
        ],
    ),
    **render_state_entries(
        ENVIRONMENT_SWITCH,
        [
            "css/style/flush.rs:ENABLED",
            "css/style/mod.rs:CASCADE_WINNERS",
            "css/style/mod.rs:PREFIX_RELATION",
            "css/style/mod.rs:PUBLISHED_STYLE_TRANSACTION",
            "css/style/mod.rs:SELECTOR_TRUTH_DERIVATION",
            "css/style/mod.rs:STYLE_ANSWER_PATCH",
            "css/style/mod.rs:STYLE_PLAN_PROVENANCE",
            "layout/fc_run_cache.rs:MODE",
            "layout/update_layout.rs:ENABLED",
        ],
    ),
    **render_state_entries(
        IDENTITY,
        [
            "css/declaration_block.rs:NEXT_DECLARATION_BLOCK_IDENTITY",
            "css/rule.rs:NEXT_RULE_IDENTITY",
            "css/selector.rs:NEXT_SELECTOR_ID",
            "css/style/index.rs:NEXT",
            "css/style/prefix.rs:NEXT",
            "css/style_sheet.rs:NEXT_SHEET_IDENTITY",
            "layout/fragment_tree.rs:NEXT_IDENTITY",
            "render_state/owner.rs:NEXT",
        ],
    ),
    **render_state_entries(
        "the render owner's own states; only the owner's thread reaches its documents' render states",
        ["render_state/owner.rs:STATES"],
    ),
    **render_state_entries(
        BUILT_ONCE,
        [
            "css/computed_values.rs:FIELD_DESCRIPTORS",
            "css/computed_values.rs:PROPERTY_DEPENDENCY_MASKS",
            "css/computed_values.rs:REGISTRY",
            "css/counter_representation.rs:DECIMAL",
            "css/css_string.rs:EMPTY",
            "css/custom_properties.rs:EMPTY",
            "css/parser/stylesheet_cache.rs:HASHER",
            "css/style/publication.rs:REMAINING",
            "css/style_compute.rs:INITIAL_VALUE_TABLE",
            "css/style_compute.rs:KINDS",
            "css/style_compute.rs:LONGHANDS",
            "css/style_compute.rs:PHASE_BOUNDARIES",
            "css/style_compute.rs:PX",
        ],
    ),
    **render_state_entries(
        LOCKED,
        [
            "css/parser/arbitrary_substitution.rs:ATTR_NAMES_READ",
            "css/parser/arbitrary_substitution.rs:ATTR_NAMES_READ_GENERATION",
            "css/parser/stylesheet_cache.rs:CACHE",
            "css/style/atoms.rs:GLOBAL_ATOMS",
            "css/style/user_agent_selectors.rs:PROGRAMS",
        ],
    ),
    **render_state_entries(
        TEST_ONLY,
        [
            "css/computed_values.rs:GROUPS",
            "css/declaration_block.rs:DECLARATION_OWNER_ALLOCATIONS",
            "css/descriptor_block.rs:DESCRIPTOR_OWNER_ALLOCATIONS",
            "css/rule.rs:RULE_OWNER_ALLOCATIONS",
            "css/style/atoms.rs:GLOBAL_ATOM_TEST_LOCK",
            "css/style/font_resolution.rs:RESOLVES",
            "css/style/mod.rs:SELECTOR_TRUTH_DERIVATION_OVERRIDE",
            "css/style_compute.rs:FLY_STRINGS",
            "css/style_compute.rs:FONT_CASCADE_LIST_UNREFS",
            "css/style_compute.rs:NEXT",
        ],
    ),
    f"{RENDER_STATE_CRATE}/src/css/parser/stylesheet_cache.rs:PARSE_DEPENDENCIES": "per-thread record of the parse running on this thread; scoped to that parse",
    f"{RENDER_STATE_CRATE}/src/css/style_value.rs:VALUES": "built-once keyword values, and a replay-only table of the same name",
    f"{RENDER_STATE_CRATE}/src/painting/recording_slot.rs:RECORDING_HOLD": "a test's hold on the next recording that flies, behind a mutex; only internals arms it",
    f"{RENDER_STATE_CRATE}/src/painting/recording_slot.rs:RECORDING_HOLD_RELEASED": "wakes the recording a test held once it lets it go",
    f"{RENDER_STATE_CRATE}/src/stage_thread.rs:THREAD_SETUP": "set once before the first stage thread starts, which runs it; read-only after",
    f"{RENDER_STATE_CRATE}/src/stage_thread.rs:FLIGHT_FINISHED": "set once before the first job is submitted, which a stage thread calls when one finishes; read-only after",
    f"{RENDER_STATE_CRATE}/src/stage_thread.rs:STYLE_LAYOUT_THREAD": "the process's one StyleLayout thread, which every thread hands its jobs to",
    f"{RENDER_STATE_CRATE}/src/stage_thread.rs:PAINT_THREAD": "the process's one Paint thread, which every thread hands its jobs to",
    f"{RENDER_STATE_CRATE}/src/stage_thread.rs:THREAD": "test only",
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


def state_in(root, source_directory, every_thread_local=False):
    """Yields `(relative path, line number, name)` for every writable static in the crate's sources, and
    every `thread_local!` where `every_thread_local`, whatever its type."""
    for source in sorted(source_directory.glob("**/*.rs")):
        relative = source.relative_to(root).as_posix()
        in_thread_local = False
        for number, line in enumerate(source.read_text().splitlines(), start=1):
            if re.match(r"^\s*thread_local!\s*\{", line):
                in_thread_local = True
                continue
            if in_thread_local and re.match(r"^\s*\}", line):
                in_thread_local = False
            match = STATE.match(line)
            if not match:
                continue
            # A shared reference to constant data is not state: nothing can write it.
            if match.group(3).lstrip().startswith("&"):
                continue
            if match.group(1) or INTERIOR_MUTABILITY.search(match.group(3)) or (every_thread_local and in_thread_local):
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

    render_state_failures = []
    render_state_present = set()
    for relative, number, name in state_in(root, root / RENDER_STATE_CRATE / "src", every_thread_local=True):
        render_state_present.add(f"{relative}:{name}")
        if f"{relative}:{name}" not in RENDER_STATE_ALLOWED:
            render_state_failures.append(f"{relative}:{number}: {name}")
    for entry in sorted(RENDER_STATE_ALLOWED.keys() - render_state_present):
        render_state_failures.append(f"{entry}: listed in RENDER_STATE_ALLOWED but no longer present")

    if not failures and not render_state_failures:
        return 0
    if failures:
        print("A Rust crate compiled into more than one library must keep no process-wide state:")
        for failure in failures:
            print(f"  {failure}")
        print("Move it behind C++-owned storage (see Libraries/LibGfx/RustProcessState.cpp), or add it")
        print("to ALLOWED in this script with the reason a per-copy instance is harmless.")
    if render_state_failures:
        print("A static or thread_local in libweb_rust is state another thread shares or copies:")
        for failure in render_state_failures:
            print(f"  {failure}")
        print("Move it into a document's render state, or add it to RENDER_STATE_ALLOWED in this")
        print("script with the reason it is fine on a thread other than the document's.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
