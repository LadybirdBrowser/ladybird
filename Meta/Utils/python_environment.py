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
from typing import BinaryIO

LADYBIRD_SOURCE_DIR = Path(__file__).resolve().parent.parent.parent

REQUIREMENTS = LADYBIRD_SOURCE_DIR / "requirements.txt"
CACHE_ROOT = LADYBIRD_SOURCE_DIR / "Build" / "venv"


def lock_environment(lock_file: BinaryIO) -> None:
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


def environment_path(requirements: bytes, cache_root: Path) -> Path:
    # Use the base interpreter so running this script from the managed environment selects the same environment as
    # running it from the system Python.
    interpreter = os.path.realpath(getattr(sys, "_base_executable", sys.executable))
    identity = b"\0".join((requirements, sys.version.encode(), os.fsencode(interpreter), os.fsencode(cache_root)))
    return cache_root / hashlib.sha256(identity).hexdigest()[:16]


def prepare_environment(requirements_path: Path = REQUIREMENTS, cache_root: Path = CACHE_ROOT) -> Path:
    requirements = requirements_path.read_bytes()
    environment = environment_path(requirements, cache_root)

    binaries = environment / ("Scripts" if os.name == "nt" else "bin")
    python = binaries / ("python.exe" if os.name == "nt" else "python")

    ready = environment / ".ready"
    if ready.is_file() and python.is_file():
        return binaries

    cache_root.mkdir(parents=True, exist_ok=True)
    with (cache_root / f"{environment.name}.lock").open("a+b") as lock_file:
        lock_environment(lock_file)

        if not ready.is_file() or not python.is_file():
            venv.EnvBuilder(with_pip=True, clear=True).create(environment)

            # Install the exact snapshot used for the cache key, even if the checkout changes while pip is running.
            snapshot = environment / "requirements.txt"
            snapshot.write_bytes(requirements)

            subprocess.run(
                [
                    str(python),
                    "-m",
                    "pip",
                    "install",
                    "--quiet",
                    "--quiet",
                    "--disable-pip-version-check",
                    "--require-virtualenv",
                    "--requirement",
                    str(snapshot),
                ],
                check=True,
                stdout=sys.stderr,
            )

            ready.touch()

    return binaries


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare the shared Python environment for tests and linters.")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--print-python", action="store_true", help="Print the interpreter path without preparing it")
    output.add_argument("--print-bin", action="store_true", help="Print the binary directory after preparing it")
    args = parser.parse_args()

    if args.print_python:
        environment = environment_path(REQUIREMENTS.read_bytes(), CACHE_ROOT)
        binaries = environment / ("Scripts" if os.name == "nt" else "bin")
        print((binaries / ("python.exe" if os.name == "nt" else "python")).as_posix())
    else:
        binaries = prepare_environment()
        if args.print_bin:
            print(binaries.as_posix())


if __name__ == "__main__":
    main()
