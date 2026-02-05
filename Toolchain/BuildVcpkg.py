#!/usr/bin/env python3

# Copyright (c) 2024, pheonixfirewingz <luke.a.shore@proton.me>
# Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
#
# SPDX-License-Identifier: BSD-2-Clause

import json
import pathlib
import subprocess
import sys

LADYBIRD_SOURCE_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.append(str(LADYBIRD_SOURCE_DIR))

from Meta.host_platform import HostSystem  # noqa: E402
from Meta.host_platform import Platform  # noqa: E402


def build_vcpkg():
    platform = Platform()

    with open(LADYBIRD_SOURCE_DIR / "vcpkg.json", "r") as vcpkg_json_file:
        vcpkg_json = json.load(vcpkg_json_file)

    git_repo = "https://github.com/microsoft/vcpkg.git"
    git_rev = vcpkg_json["builtin-baseline"]

    build_dir = LADYBIRD_SOURCE_DIR / "Build"
    build_dir.mkdir(parents=True, exist_ok=True)
    vcpkg_checkout = build_dir / "vcpkg"

    if not vcpkg_checkout.is_dir():
        subprocess.check_call(args=["git", "clone", git_repo], cwd=build_dir)
    else:
        bootstrapped_vcpkg_version = (
            subprocess.check_output(["git", "-C", vcpkg_checkout, "rev-parse", "HEAD"]).strip().decode()
        )

        if bootstrapped_vcpkg_version == git_rev:
            return

    print(f"Building vcpkg@{git_rev}")

    subprocess.check_call(args=["git", "fetch", "origin"], cwd=vcpkg_checkout)
    subprocess.check_call(args=["git", "checkout", git_rev], cwd=vcpkg_checkout)

    bootstrap_script = "bootstrap-vcpkg.bat" if platform.host_system == HostSystem.Windows else "bootstrap-vcpkg.sh"
    arguments = [vcpkg_checkout / bootstrap_script, "-disableMetrics"]

    if platform.libc_name() == "musl":
        arguments.append("-musl")

    subprocess.check_call(args=arguments, cwd=vcpkg_checkout)


def main():
    build_vcpkg()


if __name__ == "__main__":
    main()
