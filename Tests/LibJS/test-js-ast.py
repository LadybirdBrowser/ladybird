#!/usr/bin/env python3

import difflib
import os
import subprocess
import sys

from argparse import ArgumentParser
from concurrent.futures import ThreadPoolExecutor
from concurrent.futures import as_completed
from pathlib import Path

LADYBIRD_SOURCE_DIR: Path
AST_TEST_DIR: Path
BUILD_DIR: Path


def setup() -> None:
    global LADYBIRD_SOURCE_DIR, AST_TEST_DIR, BUILD_DIR

    ladybird_source_dir = os.getenv("LADYBIRD_SOURCE_DIR")

    if ladybird_source_dir is None:
        print("LADYBIRD_SOURCE_DIR must be set!")
        sys.exit(1)

    LADYBIRD_SOURCE_DIR = Path(ladybird_source_dir)
    AST_TEST_DIR = LADYBIRD_SOURCE_DIR / "Tests/LibJS/AST/"

    # The script is copied to bin/test-js-ast, so the build dir is one level up
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
    args = [
        str(BUILD_DIR / "bin/js"),
        str(AST_TEST_DIR / "input" / file),
        "--disable-ansi-colors",
        "--dump-ast",
        "--parse-only",
    ]
    process = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    stdout = process.stdout.decode().strip()

    if process.returncode != 0:
        print(stdout)
        sys.exit(1)

    expected_file = AST_TEST_DIR / "expected" / file.with_suffix(".txt")
    output_file = AST_TEST_DIR / "output" / file.with_suffix(".txt")

    output_file.write_text(stdout, encoding="utf8")

    if rebaseline:
        expected_file.write_text(stdout, encoding="utf8")
        return False

    expected = expected_file.read_text(encoding="utf8").strip()

    if stdout != expected:
        print(f"\nAST does not match for {file}!\n")

        diff(a=expected, a_file=expected_file, b=stdout, b_file=output_file)

        return True

    return False


def main() -> int:
    setup()

    parser = ArgumentParser()
    parser.add_argument("-j", "--jobs", type=int)
    parser.add_argument("--rebaseline", action="store_true")

    args = parser.parse_args()

    input_dir = AST_TEST_DIR / "input"
    failed = 0

    js_files = [js_file for js_file in sorted(input_dir.iterdir()) if js_file.is_file() and js_file.suffix == ".js"]

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        executables = [executor.submit(test, js_file.relative_to(input_dir), args.rebaseline) for js_file in js_files]

        for executable in as_completed(executables):
            if executable.result():
                failed += 1

    total = len(js_files)
    passed = total - failed

    if args.rebaseline:
        print(f"Rebaselined {total} tests.")
        return 0

    if failed:
        print(f"\nTests: {passed} passed, {failed} failed, {total} total")
        return 1

    print(f"All tests passed! ({total} total)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
