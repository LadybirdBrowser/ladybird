#!/usr/bin/env python3

# Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import re
import shutil
import sys

from pathlib import Path
from typing import List
from typing import Optional

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Meta.host_platform import HostSystem
from Meta.host_platform import Platform
from Meta.utils import run_command

CLANG_FORMAT_MAJOR_VERSION = 20
CLANG_FORMAT_EXECUTABLE = "clang-format"


def get_clang_format_version(clang_format_path: str) -> Optional[int]:
    version_output = run_command([clang_format_path, "--version"], return_output=True)
    if not version_output:
        return None

    version_match = re.search(r"version\s+(\d+)\.\d+\.\d+?", version_output)
    if version_match:
        return int(version_match.group(1))

    return None


def find_clang_format() -> Optional[str]:
    versioned_name = f"{CLANG_FORMAT_EXECUTABLE}-{CLANG_FORMAT_MAJOR_VERSION}"
    if shutil.which(versioned_name):
        return versioned_name

    if shutil.which("brew"):
        brew_prefix = run_command(["brew", "--prefix", f"llvm@{CLANG_FORMAT_MAJOR_VERSION}"], return_output=True)
        if brew_prefix:
            brew_clang_format = Path(brew_prefix.strip()) / "bin" / CLANG_FORMAT_EXECUTABLE
            if brew_clang_format.exists():
                return str(brew_clang_format)

    if shutil.which(CLANG_FORMAT_EXECUTABLE):
        version = get_clang_format_version(CLANG_FORMAT_EXECUTABLE)
        if version != CLANG_FORMAT_MAJOR_VERSION:
            print(
                f"You are using {CLANG_FORMAT_EXECUTABLE} version {version}, which appears to not be {CLANG_FORMAT_EXECUTABLE} {CLANG_FORMAT_MAJOR_VERSION}.",
                file=sys.stderr,
            )
            print("It is very likely that the resulting changes are not what you wanted.", file=sys.stderr)
        return CLANG_FORMAT_EXECUTABLE

    return None


def get_files_to_format(use_git: bool, additional_files: List[str]) -> List[str]:
    if use_git:
        git_output = run_command(
            [
                "git",
                "ls-files",
                "--",
                "*.cpp",
                "*.h",
                "*.mm",
                ":!:Base",
            ],
            return_output=True,
        )

        if not git_output:
            return []

        return [f.strip() for f in git_output.strip().split("\n") if f.strip()]
    else:
        return [file for file in additional_files if file.endswith((".cpp", ".h", ".mm"))]


def main():
    parser = argparse.ArgumentParser(description=f"Format C/C++ files using {CLANG_FORMAT_EXECUTABLE}")
    parser.add_argument("--overwrite-inplace", action="store_true", help="Overwrite files in place", required=True)
    parser.add_argument("files", nargs="*", help="Additional files to format (if none provided, uses git ls-files)")

    args = parser.parse_args()

    use_git = len(args.files) == 0
    files = get_files_to_format(use_git, args.files)

    if not files:
        print("No .cpp, .h or .mm files to check.")
        return

    clang_format = find_clang_format()
    if not clang_format:
        print(
            f"{CLANG_FORMAT_EXECUTABLE}-{CLANG_FORMAT_MAJOR_VERSION} is not available, but C or C++ files need linting! "
            f"Either skip this script, or install {CLANG_FORMAT_EXECUTABLE}-{CLANG_FORMAT_MAJOR_VERSION}.",
            file=sys.stderr,
        )
        print(
            f"(If you install a package '{CLANG_FORMAT_EXECUTABLE}', please make sure it's version {CLANG_FORMAT_MAJOR_VERSION}.)",
            file=sys.stderr,
        )
        sys.exit(1)

    print(f"Using {clang_format}")

    # Windows doesn't seem to be able to handle executing a command this long: https://devblogs.microsoft.com/oldnewthing/?p=41553.
    # So we'll execute the files in chunks of 10 at a time to ensure regardless of how much the codebase scales this command will
    # succeed
    batch_size = 10 if Platform().host_system == HostSystem.Windows else len(files)
    for i in range(0, len(files), batch_size):
        batch = files[i : i + batch_size]
        cmd = [clang_format, "-style=file", "-i"] + batch
        run_command(cmd, exit_on_failure=True)

    print(f"Maybe some files have changed. Sorry, but {CLANG_FORMAT_EXECUTABLE} doesn't indicate what happened.")


if __name__ == "__main__":
    main()
