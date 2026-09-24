#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

"""Pack an installed Ladybird.app into a tarball such that it's runnable on a machine other than the one which built it.

Give every executable bundle-relative rpaths; drop absolute ones; copy in the libraries the bundle actually links
(resolving each @rpath reference the way dyld does); re-sign the executables with the entitlements the build gave them;
check that nothing still points outside the bundle; and tar the result.

CI invokes this on the macOS Sanitizer build machine to produce a Ladybird with which we can run the AppKit AX tests,
but on a different machine — without needing to re-build Ladybird on that other machine.

Otherwise, without this, the bundle runs only on the machine which built it. That's because "cmake --install" on its own
leaves every binary with only the absolute rpaths it was built with (the vcpkg library directory, the build's lib/ — and
for a sanitizer build, the compiler's runtime directory); leaves behind the dylibs it links from those directories; and
"--strip" rewrites every executable after the build signed it — invalidating signatures.

    Meta/package-macos-app-bundle.py --build-dir Build/release --install-dir /tmp/Install --output /tmp/Ladybird.tar.gz
"""

from __future__ import annotations

import argparse
import functools
import os
import re
import shutil
import subprocess
import sys

from pathlib import Path
from typing import Iterable
from typing import Optional

MACH_O_MAGICS = (
    b"\xcf\xfa\xed\xfe",  # MH_MAGIC_64 (little endian)
    b"\xce\xfa\xed\xfe",  # MH_MAGIC
    b"\xca\xfe\xba\xbe",  # FAT_MAGIC
    b"\xbe\xba\xfe\xca",  # FAT_CIGAM
)

# Libraries dyld finds on every macOS; nothing to copy.
SYSTEM_LIBRARY_PREFIXES = ("/usr/lib/", "/System/Library/")

# The rpaths every executable in the bundle gets: the lagom libraries "cmake --install" lays down and the libraries
# this script copies live in Contents/lib, and the install puts libssl and libcrypto in Contents/Frameworks.
BUNDLE_RPATHS = ("@executable_path/../lib", "@executable_path/../Frameworks")


def is_mach_o(path: Path) -> bool:
    if path.is_symlink() or not path.is_file():
        return False
    with path.open("rb") as file:
        return file.read(4) in MACH_O_MAGICS


def run(*command: str) -> str:
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def linked_libraries(binary: Path) -> list[str]:
    """The install names of every library `binary` links, as `otool -L` prints them."""
    names = []
    for line in run("otool", "-L", str(binary)).splitlines()[1:]:
        match = re.match(r"\s+(\S+) \(", line)
        if match:
            names.append(match.group(1))
    # A dylib's own install name leads its list; a library never links itself.
    return [name for name in names if Path(name).name != binary.name]


@functools.lru_cache(maxsize=None)
def rpath_entries(binary: Path) -> list[str]:
    """The LC_RPATH entries of `binary`, in load order."""
    entries = []
    in_rpath_command = False
    for line in run("otool", "-l", str(binary)).splitlines():
        if line.strip() == "cmd LC_RPATH":
            in_rpath_command = True
        elif in_rpath_command and line.strip().startswith("path "):
            entries.append(line.strip()[len("path ") :].split(" (offset")[0])
            in_rpath_command = False
    return entries


def mach_o_files(*directories: Path) -> list[Path]:
    files = []
    for directory in directories:
        if directory.is_dir():
            files.extend(path for path in sorted(directory.rglob("*")) if is_mach_o(path))
    return files


def find_library(relative: str, search_directories: Iterable[Path]) -> Optional[Path]:
    for directory in search_directories:
        candidate = directory / relative
        if candidate.exists():
            return candidate
    return None


def expand(binary: Path, app: Path, reference: str) -> Path:
    """Expand dyld's @executable_path and @loader_path in `reference`, for a library that `binary` loads."""
    expanded = reference.replace("@executable_path", str(app / "Contents" / "MacOS"), 1)
    expanded = expanded.replace("@loader_path", str(binary.parent), 1)
    return Path(os.path.normpath(expanded))


def resolve_in_bundle(binary: Path, app: Path, name: str) -> Optional[Path]:
    """Where dyld finds the library `name` for `binary` inside the bundle, or None if it wouldn't.

    An @rpath reference is resolved the way dyld resolves it, against the rpath stack of the process: the executable's
    own entries, then those of each dylib on the load chain. An executable in Contents/MacOS is its own process, so it
    gets only its own rpaths — a helper inherits nothing from the main executable. A dylib is loaded by one of those
    executables, every one of which carries BUNDLE_RPATHS once fix_rpaths() has run, so it resolves against those plus
    its own. An absolute rpath never counts, and neither does an absolute reference outside the bundle: both name the
    build tree, which the machine that unpacks the tarball doesn't have."""
    contents = app / "Contents"
    if not name.startswith("@rpath/"):
        candidate = expand(binary, app, name)
        return candidate if candidate.exists() and candidate.is_relative_to(contents) else None
    rpaths = [entry for entry in rpath_entries(binary) if not entry.startswith("/")]
    if binary.parent != contents / "MacOS":
        rpaths = [*BUNDLE_RPATHS, *rpaths]
    for entry in rpaths:
        candidate = expand(binary, app, entry) / name[len("@rpath/") :]
        if candidate.exists():
            return candidate
    return None


def bundle_binaries(app: Path) -> list[Path]:
    return mach_o_files(app / "Contents" / "MacOS", app / "Contents" / "lib", app / "Contents" / "Frameworks")


def fix_rpaths(app: Path) -> list[Path]:
    """Give every executable the bundle's relative rpaths, and strip the build tree's absolute ones from every binary.

    "cmake --install" leaves each binary with only the absolute rpaths it was built with, which is why the installed
    bundle runs on the machine that built it and nowhere else. dyld resolves @rpath against the rpaths of the process's
    executable and of the dylibs on its load chain, so a helper process sees nothing of the main executable's rpaths:
    every executable needs the relative entries itself. The absolute entries go, too — so that a bundle missing a
    library fails on the machine that built it, rather than only on the one that unpacks the tarball. Returns the
    binaries install_name_tool rewrote; their signatures are stale until resign() runs."""
    executables_directory = app / "Contents" / "MacOS"
    rewritten = []
    for binary in bundle_binaries(app):
        entries = rpath_entries(binary)
        absolute = [entry for entry in entries if entry.startswith("/")]
        missing = (
            [entry for entry in BUNDLE_RPATHS if entry not in entries] if binary.parent == executables_directory else []
        )
        if not absolute and not missing:
            continue
        # One -delete_rpath removes one entry, and the build leaves duplicates, so delete each occurrence in turn.
        for entry in absolute:
            run("install_name_tool", "-delete_rpath", entry, str(binary))
        if missing:
            run(
                "install_name_tool", *(argument for entry in missing for argument in ("-add_rpath", entry)), str(binary)
            )
        rewritten.append(binary)
    rpath_entries.cache_clear()
    return rewritten


def copy_linked_libraries(app: Path, search_directories: list[Path]) -> list[str]:
    """Copy every library the bundle links through @rpath but doesn't carry into Contents/lib, transitively."""
    library_directory = app / "Contents" / "lib"
    library_directory.mkdir(parents=True, exist_ok=True)
    copied = []
    pending = bundle_binaries(app)
    seen = {binary.resolve() for binary in pending}
    while pending:
        binary = pending.pop()
        for name in linked_libraries(binary):
            if name.startswith(SYSTEM_LIBRARY_PREFIXES):
                continue
            library = resolve_in_bundle(binary, app, name)
            if library is None:
                if not name.startswith("@rpath/"):
                    sys.exit(f"error: {binary.relative_to(app)} links {name}, which names nothing inside the bundle")
                relative = name[len("@rpath/") :]
                source = find_library(relative, search_directories)
                if source is None:
                    sys.exit(f"error: {binary.relative_to(app)} links {name}, and no search directory has it")
                # Contents/lib is on every executable's rpath (fix_rpaths), so a copied library resolves from there.
                # Dereference vcpkg's version symlinks: the bundle carries the file under the name that is linked.
                library = library_directory / relative
                library.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source.resolve(), library)
                copied.append(relative)
            if library.resolve() not in seen and is_mach_o(library.resolve()):
                seen.add(library.resolve())
                pending.append(library.resolve())
    return copied


def resign(app: Path, build_directory: Path, rewritten: list[Path]) -> list[str]:
    """Re-sign every dylib install_name_tool rewrote ad hoc, then the helpers ad hoc with the entitlements the build
    gave them (see ladybird_helper_process), then the bundle as a whole, which signs the main executable and reseals
    the bundle over the libraries copied in."""
    executables_directory = app / "Contents" / "MacOS"
    main_executable = executables_directory / "Ladybird"
    resigned = []
    for library in rewritten:
        if library.parent != executables_directory:
            subprocess.run(["codesign", "--sign", "-", "--force", str(library)], check=True, capture_output=True)
    for executable in mach_o_files(executables_directory):
        if executable == main_executable:
            continue
        command = ["codesign", "--sign", "-", "--force"]
        entitlements = build_directory / "Services" / executable.name / f"{executable.name}.entitlements"
        if entitlements.is_file():
            command += ["--options", "runtime", "--entitlements", str(entitlements)]
        subprocess.run([*command, str(executable)], check=True, capture_output=True)
        resigned.append(executable.name)
    subprocess.run(["codesign", "--sign", "-", "--force", str(app)], check=True, capture_output=True)
    resigned.append(app.name)
    return resigned


def verify(app: Path) -> None:
    """Fail if any binary still has a build-tree rpath, any executable lacks the bundle's, any signature is invalid, or
    any library reference still points outside the bundle."""
    executables_directory = app / "Contents" / "MacOS"
    for binary in bundle_binaries(app):
        entries = rpath_entries(binary)
        if any(entry.startswith("/") for entry in entries):
            sys.exit(f"error: {binary.relative_to(app)} still has an absolute rpath: {entries}")
        if binary.parent == executables_directory and any(entry not in entries for entry in BUNDLE_RPATHS):
            sys.exit(f"error: {binary.relative_to(app)} lacks the bundle rpaths: {entries}")
        result = subprocess.run(["codesign", "--verify", str(binary)], capture_output=True, text=True)
        if result.returncode != 0:
            sys.exit(f"error: {binary.relative_to(app)} has an invalid signature: {result.stderr.strip()}")
        for name in linked_libraries(binary):
            if name.startswith(SYSTEM_LIBRARY_PREFIXES):
                continue
            if resolve_in_bundle(binary, app, name) is None:
                sys.exit(f"error: {binary.relative_to(app)} still links {name}, which isn't in the bundle")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, type=Path, help="the CMake build directory the app was built in")
    parser.add_argument(
        "--install-dir", required=True, type=Path, help="the prefix `cmake --install --strip` installed into"
    )
    parser.add_argument("--output", required=True, type=Path, help="the .tar.gz to write, holding Ladybird.app")
    args = parser.parse_args()

    app = args.install_dir / "bundle" / "Ladybird.app"
    built_app_binary = args.build_dir / "bin" / "Ladybird.app" / "Contents" / "MacOS" / "Ladybird"
    if not (app / "Contents" / "MacOS" / "Ladybird").is_file():
        sys.exit(f"error: no installed app bundle at {app}")
    if not built_app_binary.is_file():
        sys.exit(f"error: no built app binary at {built_app_binary}")

    # The build tree's binaries carry absolute rpaths (the vcpkg library directory, the build's own lib/, and for a
    # sanitizer build the compiler's runtime directory); those are the directories dyld searched when the app ran from
    # the build tree, so they are where its libraries are.
    search_directories = [Path(entry) for entry in rpath_entries(built_app_binary) if entry.startswith("/")]
    search_directories = [directory for directory in search_directories if directory.is_dir()]
    if not search_directories:
        sys.exit(f"error: {built_app_binary} has no absolute rpath entries to search")

    # Rpaths first, so the libraries the install laid down in Contents/lib count as found and only the vcpkg ones get
    # copied; then again for what was copied, since vcpkg's dylibs can carry absolute rpaths of their own.
    rewritten = fix_rpaths(app)
    copied = copy_linked_libraries(app, search_directories)
    rewritten += fix_rpaths(app)
    resigned = resign(app, args.build_dir, rewritten)
    verify(app)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["tar", "-czf", str(args.output), "-C", str(app.parent), app.name], check=True)

    size = os.path.getsize(args.output) / (1024 * 1024)
    print(f"rewrote the rpaths of {len(rewritten)} binaries")
    print(f"copied {len(copied)} libraries into {app.relative_to(args.install_dir)}/Contents/lib")
    print(f"re-signed: {' '.join(resigned)}")
    print(f"wrote {args.output} ({size:.0f} MiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
