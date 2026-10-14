#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# Checks that every C function the Rust crates of LibWeb export has a caller among the objects of the build. Grep
# cannot tell: a C++ declaration or inline wrapper that nothing calls still names the export, but emits no code, so no
# object references the symbol.

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Dict
from typing import Iterable
from typing import List
from typing import Set
from typing import Tuple

LIBWEB_RUST_CRATES = frozenset(
    {
        "libweb_rust",
        "libweb_bindings_rust",
        "libweb_html_tokenizer",
        "libweb_content_blocker_rust",
    }
)
DEFINED_SYMBOL_KINDS = frozenset("TDSBRVW")
UNDEFINED_SYMBOL_KINDS = frozenset("Uwv")
OBJECTS_PER_NM_CALL = 256
UNBUILT_OBJECTS_LISTED = 5
ALLOWLIST_FILE_NAME = "check-dead-rust-exports-allowlist.txt"
STYLE_ENGINE_ENTRY_PREFIX = "style_engine_"
STYLE_BOUNDARY_TABLE = "Libraries/LibWeb/Rust/src/css/style/boundary.rs"

# A line of `nm -P -A`: "<file>: <name> <kind> [<value> [<size>]]", where <file> is "<archive>[<member>]" for a member
# of an archive. GNU nm leaves out the value and size of an undefined symbol, and an Objective-C name may hold ": " and
# spaces.
NM_SYMBOL_LINE = re.compile(r"(?P<file>.+?): (?P<name>.+?) (?P<kind>[A-Za-z?-])(?: [0-9a-fA-F]+){0,2}\s*")
ARCHIVE_MEMBER = re.compile(r".+\[(?P<member>[^\]]+)\]")
C_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


@dataclass(frozen=True)
class NmSymbol:
    file: str
    name: str
    kind: str


@dataclass(frozen=True)
class BuildObject:
    path: str
    source: Path


@dataclass(frozen=True)
class RustImport:
    name: str
    crate: str


@dataclass
class RustArchiveSymbols:
    exporting_crate_by_symbol: Dict[str, str]
    imports: List[RustImport]


def name_as_written_in_source(name: str) -> str:
    # Mach-O prefixes every C name with an underscore.
    if sys.platform == "darwin" and name.startswith("_"):
        return name[1:]
    return name


def is_c_name(name: str) -> bool:
    return C_IDENTIFIER.fullmatch(name) is not None and not name.startswith(("_R", "_Z"))


def nm_command(nm: str) -> List[str]:
    # llvm-nm reads the symbols of an object that embeds LLVM bitcode, as the Rust standard library's do, out of the
    # bitcode, which fails when rustc's LLVM is newer than the toolchain's.
    help_result = subprocess.run([nm, "--help"], capture_output=True, text=True)
    if "--no-llvm-bc" in help_result.stdout + help_result.stderr:
        return [nm, "--no-llvm-bc"]
    return [nm]


def run_nm(nm: List[str], paths: List[str]) -> List[NmSymbol]:
    result = subprocess.run([*nm, "-P", "-A", "-g", *paths], capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(f"error: {' '.join(nm)} failed: {result.stderr.strip()}")
    symbols = []
    for line in result.stdout.splitlines():
        match = NM_SYMBOL_LINE.fullmatch(line)
        if match is None:
            continue
        name = name_as_written_in_source(match["name"])
        if is_c_name(name):
            symbols.append(NmSymbol(match["file"], name, match["kind"]))
    return symbols


def read_rust_archives(nm: List[str], archives: List[Path]) -> RustArchiveSymbols:
    exporting_crate_by_symbol: Dict[str, str] = {}
    imports: List[RustImport] = []
    for archive in archives:
        if not archive.is_file():
            raise SystemExit(f"error: {archive} does not exist; build LibWeb first")
        for symbol in run_nm(nm, [str(archive)]):
            member_match = ARCHIVE_MEMBER.fullmatch(symbol.file)
            if member_match is None:
                continue
            # A member of a Rust archive is named "<crate>-<hash>.<crate>.<hash>-cgu.<n>.rcgu.o".
            crate = member_match["member"].split("-", 1)[0]
            if symbol.kind in UNDEFINED_SYMBOL_KINDS:
                imports.append(RustImport(symbol.name, crate))
            elif symbol.kind in DEFINED_SYMBOL_KINDS and crate in LIBWEB_RUST_CRATES:
                exporting_crate_by_symbol[symbol.name] = crate
    return RustArchiveSymbols(exporting_crate_by_symbol, imports)


def read_build_objects(compile_commands: Path) -> List[BuildObject]:
    objects = []
    for entry in json.loads(compile_commands.read_text()):
        if "output" not in entry:
            raise SystemExit(f"error: {compile_commands} names no output of {entry['file']}; CMake 3.20 writes it")
        directory = Path(entry["directory"])
        objects.append(BuildObject(str(directory / entry["output"]), (directory / entry["file"]).resolve()))
    return objects


def read_objects(nm: List[str], objects: List[BuildObject]) -> List[NmSymbol]:
    paths = [build_object.path for build_object in objects]
    batches = [paths[start : start + OBJECTS_PER_NM_CALL] for start in range(0, len(paths), OBJECTS_PER_NM_CALL)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as executor:
        return [symbol for symbols in executor.map(lambda batch: run_nm(nm, batch), batches) for symbol in symbols]


def read_allowlist(allowlist: Path) -> Tuple[Set[str], List[str]]:
    allowed = set()
    errors = []
    if not allowlist.is_file():
        return allowed, errors
    for line_number, line in enumerate(allowlist.read_text().splitlines(), start=1):
        symbol, _, reason = line.partition("#")
        symbol = symbol.strip()
        if not symbol:
            continue
        if not reason.strip():
            errors.append(f"{allowlist.name}:{line_number}: {symbol} needs a comment saying why it is alive")
        allowed.add(symbol)
    return allowed, errors


def git_grep(source_root: Path, arguments: List[str]) -> List[str]:
    try:
        result = subprocess.run(
            ["git", "-C", str(source_root), "grep", "-n", *arguments], capture_output=True, text=True
        )
    except OSError:
        return []
    return result.stdout.splitlines()


def export_source_hints(source_root: Path, name: str) -> List[str]:
    hints = git_grep(source_root, ["-w", "-F", f"fn {name}", "--", "*.rs"])
    if name.startswith(STYLE_ENGINE_ENTRY_PREFIX):
        row = name[len(STYLE_ENGINE_ENTRY_PREFIX) :]
        hints += git_grep(source_root, ["-w", "-E", f"^[[:space:]]*{row}", "--", STYLE_BOUNDARY_TABLE])
    hints += git_grep(source_root, ["-w", "-F", name, "--", "*.h", "*.cpp", "*.mm"])
    return hints


def path_for_display(path: Path, source_root: Path) -> str:
    return str(path.relative_to(source_root)) if path.is_relative_to(source_root) else str(path)


def print_findings(heading: str, findings: Iterable[Tuple[str, List[str]]]) -> None:
    print(f"error: {heading}")
    for finding, details in findings:
        print(f"  {finding}")
        for detail in details:
            print(f"      {detail}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Check that every Rust export of LibWeb has a C++ caller.")
    parser.add_argument("--nm", required=True, help="the nm of the toolchain")
    parser.add_argument("--compile-commands", required=True, type=Path, help="the compile_commands.json of the build")
    parser.add_argument("--source-root", required=True, type=Path, help="the root of the source tree")
    parser.add_argument(
        "--allowlist",
        type=Path,
        default=Path(__file__).resolve().with_name(ALLOWLIST_FILE_NAME),
        help="exports known to have no C++ caller, one per line with a comment saying why",
    )
    parser.add_argument("archives", nargs="+", type=Path, help="the static libraries of LibWeb's Rust crates")
    arguments = parser.parse_args()

    source_root: Path = arguments.source_root.resolve()
    build_directory = arguments.compile_commands.resolve().parent
    libweb_source_directories = [source_root / "Libraries" / "LibWeb", build_directory / "Libraries" / "LibWeb"]
    tests_directory = source_root / "Tests"

    nm = nm_command(arguments.nm)
    rust = read_rust_archives(nm, arguments.archives)
    all_objects = read_build_objects(arguments.compile_commands)
    built_objects = [build_object for build_object in all_objects if os.path.isfile(build_object.path)]
    unbuilt_objects = [build_object for build_object in all_objects if not os.path.isfile(build_object.path)]
    source_by_object = {build_object.path: build_object.source for build_object in built_objects}

    referencing_objects_by_symbol: Dict[str, Set[str]] = defaultdict(set)
    libweb_cpp_definitions: List[NmSymbol] = []
    for symbol in read_objects(nm, built_objects):
        if symbol.kind in UNDEFINED_SYMBOL_KINDS:
            referencing_objects_by_symbol[symbol.name].add(symbol.file)
        elif symbol.kind == "T" and any(
            source_by_object[symbol.file].is_relative_to(directory) for directory in libweb_source_directories
        ):
            libweb_cpp_definitions.append(symbol)

    allowlisted, errors = read_allowlist(arguments.allowlist)
    for error in errors:
        print(f"error: {error}")

    exports_without_caller = sorted(
        name for name in rust.exporting_crate_by_symbol if name not in referencing_objects_by_symbol
    )
    dead_exports = [name for name in exports_without_caller if name not in allowlisted]
    if dead_exports:
        print_findings(
            f"{len(dead_exports)} Rust exports of LibWeb have no caller among the objects of the build. Delete each "
            f"export and whatever names it in C++, or list it in {arguments.allowlist.name} with the reason it is "
            "alive:",
            (
                (f"{name} ({rust.exporting_crate_by_symbol[name]})", export_source_hints(source_root, name))
                for name in dead_exports
            ),
        )

    stale_allowlist_entries = sorted(
        name
        for name in allowlisted
        if name not in rust.exporting_crate_by_symbol or name in referencing_objects_by_symbol
    )
    if stale_allowlist_entries:
        print_findings(
            f"{arguments.allowlist.name} lists symbols that are not dead Rust exports of LibWeb; remove them:",
            ((name, []) for name in stale_allowlist_entries),
        )

    imports_through_c_names = sorted(
        {
            (rust_import.name, rust_import.crate, rust.exporting_crate_by_symbol[rust_import.name])
            for rust_import in rust.imports
            if rust_import.name in rust.exporting_crate_by_symbol
            and rust.exporting_crate_by_symbol[rust_import.name] != rust_import.crate
        }
    )
    if imports_through_c_names:
        print_findings(
            "Rust code calls Rust exports of another crate through their C names; call the Rust API instead:",
            (
                (f"{crate} calls {name} of {exporting_crate}", export_source_hints(source_root, name))
                for name, crate, exporting_crate in imports_through_c_names
            ),
        )

    rust_imported_symbols = {rust_import.name for rust_import in rust.imports}
    dead_cpp_definitions = sorted(
        {
            (definition.name, path_for_display(source_by_object[definition.file], source_root))
            for definition in libweb_cpp_definitions
            if definition.name not in referencing_objects_by_symbol and definition.name not in rust_imported_symbols
        }
    )
    if dead_cpp_definitions:
        print_findings(
            "C functions defined in LibWeb have no caller in C++ or Rust; delete them:",
            ((name, [source]) for name, source in dead_cpp_definitions),
        )

    exports_called_only_by_tests = sorted(
        name
        for name in rust.exporting_crate_by_symbol
        if name in referencing_objects_by_symbol
        and all(source_by_object[path].is_relative_to(tests_directory) for path in referencing_objects_by_symbol[name])
    )
    if exports_called_only_by_tests:
        print(f"note: {len(exports_called_only_by_tests)} exports are called only from Tests/:")
        for name in exports_called_only_by_tests:
            print(f"  {name}")

    if unbuilt_objects:
        print(f"warning: {len(unbuilt_objects)} objects of the build are not built, so no caller in them is counted:")
        for build_object in unbuilt_objects[:UNBUILT_OBJECTS_LISTED]:
            print(f"  {build_object.path}")

    failed = bool(errors or dead_exports or stale_allowlist_entries or imports_through_c_names or dead_cpp_definitions)
    if failed:
        if unbuilt_objects:
            print("note: the unbuilt objects may hold the missing callers; build everything and run this again.")
        return 1

    print(
        f"Each of the {len(rust.exporting_crate_by_symbol)} Rust exports of LibWeb has a caller among the "
        f"{len(built_objects)} objects of the build, or is in {arguments.allowlist.name}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
