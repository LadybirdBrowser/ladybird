#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import pathlib
import subprocess
import sys

META_SOURCE_DIR = pathlib.Path(__file__).resolve().parent.parent

sys.path.append(str(META_SOURCE_DIR))

from Utils.python_environment import prepare_environment  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description="Run the repository's pinned linters in a cached virtual environment.")
    parser.add_argument(
        "tool", nargs="?", choices=("ruff", "pyright", "actionlint"), help="Omit to prepare the environment"
    )
    parser.add_argument("args", nargs=argparse.REMAINDER, help="Arguments passed through to the linter")
    args = parser.parse_args()

    binaries = prepare_environment()
    if args.tool is None:
        print(binaries)
        return 0

    executable = binaries / (args.tool + (".exe" if os.name == "nt" else ""))
    environment = {**os.environ, "PATH": str(binaries) + os.pathsep + os.environ.get("PATH", "")}
    if args.tool == "pyright":
        # Use the pinned package's bundled checker, without version overrides
        # or network requests to look for updates.
        environment.pop("PYRIGHT_PYTHON_FORCE_VERSION", None)
        environment.pop("PYRIGHT_PYTHON_PYLANCE_VERSION", None)
        environment["PYRIGHT_PYTHON_USE_BUNDLED_PYRIGHT"] = "1"
        environment["PYRIGHT_PYTHON_IGNORE_WARNINGS"] = "1"
    return subprocess.run([str(executable), *args.args], env=environment, check=False).returncode


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"Unable to run pinned linter: {error}", file=sys.stderr)
        sys.exit(1)
