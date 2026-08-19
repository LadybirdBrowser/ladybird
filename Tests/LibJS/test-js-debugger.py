#!/usr/bin/env python3

import difflib
import os
import shlex
import subprocess
import sys

from argparse import ArgumentParser
from concurrent.futures import ThreadPoolExecutor
from concurrent.futures import as_completed
from pathlib import Path

LADYBIRD_SOURCE_DIR: Path
DEBUGGER_TEST_DIR: Path
BUILD_DIR: Path


def setup() -> None:
    global LADYBIRD_SOURCE_DIR, DEBUGGER_TEST_DIR, BUILD_DIR

    ladybird_source_dir = os.getenv("LADYBIRD_SOURCE_DIR")

    if ladybird_source_dir is None:
        print("LADYBIRD_SOURCE_DIR must be set!")
        sys.exit(1)

    LADYBIRD_SOURCE_DIR = Path(ladybird_source_dir).resolve()
    DEBUGGER_TEST_DIR = LADYBIRD_SOURCE_DIR / "Tests/LibJS/Debugger/"

    # The script is copied to bin/test-js-debugger, so the build dir is one level up
    BUILD_DIR = Path(__file__).parent.parent.resolve()


DIFF_PREFIX_ESCAPES = {
    "@": "\x1b[36m",
    "+": "\x1b[32m",
    "-": "\x1b[31m",
}


def diff(a: str, a_file: Path, b: str, b_file: Path) -> None:
    for line in difflib.unified_diff(a.splitlines(), b.splitlines(), fromfile=str(a_file), tofile=str(b_file)):
        line = line.rstrip()

        color_prefix = DIFF_PREFIX_ESCAPES.get((line or " ")[0], "")

        print(f"{color_prefix}{line}\x1b[0m")


def test(file: Path, rebaseline: bool) -> bool:
    input_file = DEBUGGER_TEST_DIR / "input" / file
    commands_file = input_file.with_suffix(".commands")
    arguments_file = input_file.with_suffix(".args")
    expected_file = DEBUGGER_TEST_DIR / "expected" / file.with_suffix(".txt")
    output_file = DEBUGGER_TEST_DIR / "output" / file.with_suffix(".txt")

    if not commands_file.is_file():
        print(f"Missing debugger commands for {file}: {commands_file}")
        return True

    if not rebaseline and not expected_file.is_file():
        print(f"Missing expected debugger output for {file}: {expected_file}")
        return True

    args = [
        str(BUILD_DIR / "bin/js"),
        "--debug",
        "--disable-ansi-colors",
    ]
    if arguments_file.is_file():
        try:
            args.extend(shlex.split(arguments_file.read_text(encoding="utf8")))
        except ValueError as error:
            print(f"Unable to parse debugger arguments for {file}: {error}")
            return True
    args.append(str(input_file))

    process = subprocess.run(
        args,
        input=commands_file.read_text(encoding="utf8"),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )

    stdout = process.stdout.replace(str(LADYBIRD_SOURCE_DIR), "<ladybird>").strip()
    output_file.write_text(stdout + "\n", encoding="utf8")

    if process.returncode != 0:
        print(f"\nDebugger exited with code {process.returncode} for {file}:\n")
        print(stdout)
        return True

    if rebaseline:
        expected_file.write_text(stdout + "\n", encoding="utf8")
        return False

    expected = expected_file.read_text(encoding="utf8").strip()

    if stdout != expected:
        print(f"\nDebugger output does not match for {file}!\n")

        diff(a=expected, a_file=expected_file, b=stdout, b_file=output_file)

        return True

    return False


def main() -> int:
    setup()

    parser = ArgumentParser()
    parser.add_argument("-j", "--jobs", type=int)
    parser.add_argument("--rebaseline", action="store_true")

    args = parser.parse_args()

    input_dir = DEBUGGER_TEST_DIR / "input"
    failed = 0

    js_files = [js_file for js_file in sorted(input_dir.iterdir()) if js_file.is_file() and js_file.suffix == ".js"]

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        executables = [executor.submit(test, js_file.relative_to(input_dir), args.rebaseline) for js_file in js_files]

        for executable in as_completed(executables):
            if executable.result():
                failed += 1

    total = len(js_files)
    passed = total - failed

    if failed:
        print(f"\nTests: {passed} passed, {failed} failed, {total} total")
        return 1

    if args.rebaseline:
        print(f"Rebaselined {total} tests.")
        return 0

    print(f"All tests passed! ({total} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
