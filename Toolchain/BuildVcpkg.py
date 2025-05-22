#!/usr/bin/env python3

import json
import os
import pathlib
import subprocess
import sys


def main() -> int:
    script_dir = pathlib.Path(__file__).parent.resolve()

    with open(script_dir.parent / "vcpkg.json", "r") as vcpkg_json_file:
        vcpkg_json = json.load(vcpkg_json_file)

    git_repo = "https://github.com/microsoft/vcpkg.git"
    git_rev = vcpkg_json["builtin-baseline"]

    build_dir = script_dir.parent / "Build"
    build_dir.mkdir(parents=True, exist_ok=True)
    vcpkg_checkout = build_dir / "vcpkg"

    if not vcpkg_checkout.is_dir():
        vcpkg_checkout.mkdir(parents=True, exist_ok=True)
        subprocess.check_call(args=["git", "init"], cwd=vcpkg_checkout)
        subprocess.check_call(
            args=["git", "remote", "add", "origin", git_repo],
            cwd=vcpkg_checkout
        )
        subprocess.check_call(
            args=["git", "fetch",
                  "--shallow-since", "2023-10-13",
                  "origin", git_rev],
            cwd=vcpkg_checkout
        )
        subprocess.check_call(
            args=["git", "checkout", git_rev],
            cwd=vcpkg_checkout
        )
    else:
        bootstrapped_vcpkg_version = (
            subprocess.check_output(["git", "-C", vcpkg_checkout, "rev-parse", "HEAD"]).strip().decode()
        )

        if bootstrapped_vcpkg_version == git_rev:
            return 0

        # clean and reset before updating
        subprocess.check_call(
            args=["git", "reset", "--hard"], cwd=vcpkg_checkout)
        subprocess.check_call(
            args=["git", "clean", "-fdx"], cwd=vcpkg_checkout)

        # get the new baseline commit
        subprocess.check_call(
            args=["git", "fetch",
                  "--shallow-since", "2023-10-13",
                  "origin", git_rev], cwd=vcpkg_checkout
        )
        subprocess.check_call(
            args=["git", "checkout", git_rev], cwd=vcpkg_checkout)

    print(f"Building vcpkg@{git_rev}")

    bootstrap_script = "bootstrap-vcpkg.bat" if os.name == 'nt' else "bootstrap-vcpkg.sh"
    subprocess.check_call(args=[vcpkg_checkout / bootstrap_script, "-disableMetrics"], cwd=vcpkg_checkout)
    bootstrap_script = "bootstrap-vcpkg.bat" if os.name == "nt" else "bootstrap-vcpkg.sh"
    subprocess.check_call(args=[vcpkg_checkout / bootstrap_script, "-disableMetrics"], cwd=vcpkg_checkout)

    return 0


if __name__ == "__main__":
    sys.exit(main())
