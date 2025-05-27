#!/usr/bin/env python3

# Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import re
import shutil
import sys

from pathlib import Path
from typing import Optional

sys.path.append(str(Path(__file__).resolve().parent.parent))

from Meta.host_platform import HostSystem
from Meta.host_platform import Platform
from Meta.utils import run_command


CLANG_MINIMUM_VERSION = 17
GCC_MINIMUM_VERSION = 13
XCODE_MINIMUM_VERSION = ("14.3", 14030022)

COMPILER_VERSION_REGEX = re.compile(r"(\d+)(\.\d+)*")


def major_compiler_version_if_supported(platform: Platform, compiler: str) -> Optional[int]:
    if not shutil.which(compiler):
        return None

    # On Windows, clang-cl is a driver that does not have the -dumpversion flag. We will use clang proper for this test.
    if platform.host_system == HostSystem.Windows:
        compiler = compiler.replace("clang-cl", "clang")

    version = run_command([compiler, "-dumpversion"], return_output=True)
    if not version:
        return None

    major_version = COMPILER_VERSION_REGEX.match(version)
    if not major_version:
        return None

    major_version = int(major_version.group(1))

    version = run_command([compiler, "--version"], return_output=True)
    if not version:
        return None

    if platform.host_system == HostSystem.macOS and version.find("Apple clang") != -1:
        apple_definitions = run_command([compiler, "-dM", "-E", "-"], input="", return_output=True)
        if not apple_definitions:
            return None

        apple_definitions = apple_definitions.split()

        try:
            index = next(i for (i, v) in enumerate(apple_definitions) if "__apple_build_version__" in v)
            apple_build_version = int(apple_definitions[index + 1])
        except (IndexError, StopIteration, ValueError):
            return None

        if apple_build_version >= XCODE_MINIMUM_VERSION[1]:
            # This inherently causes us to prefer Xcode clang over homebrew clang.
            return apple_build_version

    elif version.find("clang") != -1:
        if major_version >= CLANG_MINIMUM_VERSION:
            return major_version

    else:
        if major_version >= GCC_MINIMUM_VERSION:
            return major_version

    return None


def find_newest_compiler(platform: Platform, compilers: list[str]) -> Optional[str]:
    best_compiler = None
    best_version = 0

    for compiler in compilers:
        major_version = major_compiler_version_if_supported(platform, compiler)
        if not major_version:
            continue

        if major_version > best_version:
            best_version = major_version
            best_compiler = compiler

    return best_compiler


def pick_host_compiler(platform: Platform, cc: str, cxx: str) -> tuple[str, str]:
    if platform.host_system == HostSystem.Windows and ("clang-cl" not in cc or "clang-cl" not in cxx):
        print(
            f"clang-cl {CLANG_MINIMUM_VERSION} or higher is required on Windows",
            file=sys.stderr,
        )

        sys.exit(1)

    # FIXME: Validate that the cc/cxx combination is compatible (e.g. don't allow CC=gcc and CXX=clang++)
    if major_compiler_version_if_supported(platform, cc) and major_compiler_version_if_supported(platform, cxx):
        return (cc, cxx)

    if platform.host_system == HostSystem.Windows:
        clang_candidates = ["clang-cl"]
        gcc_candidates = []
    else:
        clang_candidates = [
            "clang",
            "clang-17",
            "clang-18",
            "clang-19",
            "clang-20",
        ]

        gcc_candidates = [
            "gcc",
            "gcc-13",
            "gcc-14",
        ]

    if platform.host_system == HostSystem.macOS:
        clang_homebrew_path = Path("/opt/homebrew/opt/llvm/bin")
        homebrew_path = Path("/opt/homebrew/bin")

        clang_candidates.extend([str(clang_homebrew_path.joinpath(c)) for c in clang_candidates])
        clang_candidates.extend([str(homebrew_path.joinpath(c)) for c in clang_candidates])

        gcc_candidates.extend([str(homebrew_path.joinpath(c)) for c in gcc_candidates])
    elif platform.host_system == HostSystem.Linux:
        local_path = Path("/usr/local/bin")

        clang_candidates.extend([str(local_path.joinpath(c)) for c in clang_candidates])
        gcc_candidates.extend([str(local_path.joinpath(c)) for c in gcc_candidates])

    clang = find_newest_compiler(platform, clang_candidates)
    if clang:
        if platform.host_system == HostSystem.Windows:
            return (clang, clang)
        return clang, clang.replace("clang", "clang++")

    gcc = find_newest_compiler(platform, gcc_candidates)
    if gcc:
        return gcc, gcc.replace("gcc", "g++")

    if platform.host_system == HostSystem.macOS:
        print(
            f"Please ensure that Xcode {XCODE_MINIMUM_VERSION[0]}, Homebrew clang {CLANG_MINIMUM_VERSION}, or higher is installed",
            file=sys.stderr,
        )
    elif platform.host_system == HostSystem.Windows:
        print(
            f"Please ensure that clang-cl {CLANG_MINIMUM_VERSION} or higher is installed",
            file=sys.stderr,
        )
    else:
        print(
            f"Please ensure that clang {CLANG_MINIMUM_VERSION}, gcc {GCC_MINIMUM_VERSION}, or higher is installed",
            file=sys.stderr,
        )

    sys.exit(1)


def default_host_compiler(platform: Platform) -> tuple[str, str]:
    if platform.host_system == HostSystem.Windows:
        return ("clang-cl", "clang-cl")
    return ("cc", "c++")


def main():
    platform = Platform()
    (default_cc, default_cxx) = default_host_compiler(platform)

    parser = argparse.ArgumentParser(description="Find valid compilers")

    parser.add_argument("--cc", required=False, default=default_cc)
    parser.add_argument("--cxx", required=False, default=default_cxx)

    args = parser.parse_args()

    # The default action when this script is invoked is to provide the caller with content that may be evaluated by bash.
    (cc, cxx) = pick_host_compiler(platform, args.cc, args.cxx)
    print(f'export CC="{cc}"')
    print(f'export CXX="{cxx}"')


if __name__ == "__main__":
    main()
