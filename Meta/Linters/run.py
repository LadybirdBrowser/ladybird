#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import hashlib
import os
import subprocess
import sys
import venv

from pathlib import Path


def lock_environment(lock_file):
    if os.name == "nt":
        import errno
        import msvcrt
        import time

        lock_file.write(b"\0")
        lock_file.flush()
        lock_file.seek(0)
        while True:
            try:
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
                return
            except OSError as error:
                if error.errno != errno.EACCES:
                    raise
                time.sleep(0.1)
    else:
        import fcntl

        fcntl.flock(lock_file, fcntl.LOCK_EX)


def prepare_environment(requirements_path, cache_root):
    requirements = requirements_path.read_bytes()
    identity = b"\0".join((requirements, sys.version.encode(), os.fsencode(sys.executable), os.fsencode(cache_root)))
    environment = cache_root / hashlib.sha256(identity).hexdigest()[:16]
    binaries = environment / ("Scripts" if os.name == "nt" else "bin")
    python = binaries / ("python.exe" if os.name == "nt" else "python")
    ready = environment / ".ready"
    if ready.is_file() and python.is_file():
        return binaries

    cache_root.mkdir(parents=True, exist_ok=True)
    with (cache_root / f"{environment.name}.lock").open("a+b") as lock_file:
        lock_environment(lock_file)
        if not ready.is_file() or not python.is_file():
            print(f"Preparing pinned linters in {environment}", file=sys.stderr, flush=True)
            venv.EnvBuilder(with_pip=True, clear=True).create(environment)
            # Install the exact snapshot used for the cache key, even if the
            # checkout changes while pip is running.
            snapshot = environment / "requirements.txt"
            snapshot.write_bytes(requirements)
            subprocess.run(
                [
                    str(python),
                    "-m",
                    "pip",
                    "install",
                    "--disable-pip-version-check",
                    "--require-virtualenv",
                    "-r",
                    str(snapshot),
                ],
                check=True,
                stdout=sys.stderr,
            )
            ready.touch()
    return binaries


def main():
    parser = argparse.ArgumentParser(description="Run the repository's pinned linters in a cached virtual environment.")
    parser.add_argument(
        "tool", nargs="?", choices=("ruff", "pyright", "actionlint"), help="Omit to prepare the environment"
    )
    parser.add_argument("args", nargs=argparse.REMAINDER, help="Arguments passed through to the linter")
    args = parser.parse_args()

    linters = Path(__file__).resolve().parent
    binaries = prepare_environment(linters / "requirements.txt", linters.parent.parent / "Build/linters")
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
