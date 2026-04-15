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
CSS_TOKENIZER_TEST_DIR: Path
BUILD_DIR: Path


def setup() -> None:
    global LADYBIRD_SOURCE_DIR, CSS_TOKENIZER_TEST_DIR, BUILD_DIR

    ladybird_source_dir = os.getenv("LADYBIRD_SOURCE_DIR")

    if ladybird_source_dir is None:
        print("LADYBIRD_SOURCE_DIR must be set!")
        sys.exit(1)

    LADYBIRD_SOURCE_DIR = Path(ladybird_source_dir)
    CSS_TOKENIZER_TEST_DIR = LADYBIRD_SOURCE_DIR / "Tests/LibWeb/CSSTokenizer/"

    # The script is copied to bin/test-css-tokenizer, so the build dir is one level up
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


def output_file_for(file: Path, backend: str) -> Path:
    return CSS_TOKENIZER_TEST_DIR / "output" / file.with_suffix(f".{backend}.txt")


def encoding_for(file: Path) -> str:
    encoding_file = CSS_TOKENIZER_TEST_DIR / "input" / Path(f"{file.name}.encoding")
    if not encoding_file.exists():
        return "utf-8"
    return encoding_file.read_text(encoding="utf8").strip()


def run_backend(file: Path, backend: str, encoding: str) -> tuple[str, Path]:
    args = [
        str(BUILD_DIR / "bin/css-tokenizer"),
        "--backend",
        backend,
        "--encoding",
        encoding,
        str(CSS_TOKENIZER_TEST_DIR / "input" / file),
    ]
    process = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    stdout = process.stdout.decode().strip()

    if process.returncode != 0:
        print(stdout)
        sys.exit(1)

    output_file = output_file_for(file, backend)
    output_file.write_text(stdout + "\n", encoding="utf8")
    return stdout, output_file


def test(file: Path, backend: str, rebaseline: bool) -> bool:
    requested_backends = ["cpp", "rust"] if backend == "both" else [backend]
    encoding = encoding_for(file)
    results = {
        requested_backend: run_backend(file, requested_backend, encoding) for requested_backend in requested_backends
    }
    expected_file = CSS_TOKENIZER_TEST_DIR / "expected" / file.with_suffix(".txt")

    if rebaseline:
        cpp_stdout = results["cpp"][0] if "cpp" in results else run_backend(file, "cpp", encoding)[0]
        expected_file.write_text(cpp_stdout + "\n", encoding="utf8")

        if "rust" in results and results["rust"][0] != cpp_stdout:
            print(f"\nRust tokenizer output does not match C++ for {file} after rebaseline!\n")
            diff(
                a=cpp_stdout,
                a_file=output_file_for(file, "cpp"),
                b=results["rust"][0],
                b_file=results["rust"][1],
            )
            return True

        return False

    expected = expected_file.read_text(encoding="utf8").strip()
    failed = False

    for current_backend, (stdout, output_file) in results.items():
        if stdout != expected:
            print(f"\nCSS tokens do not match for {file} with backend '{current_backend}'!\n")
            diff(a=expected, a_file=expected_file, b=stdout, b_file=output_file)
            failed = True

    if "cpp" in results and "rust" in results and results["cpp"][0] != results["rust"][0]:
        print(f"\nBackends disagree for {file}!\n")
        diff(
            a=results["cpp"][0],
            a_file=results["cpp"][1],
            b=results["rust"][0],
            b_file=results["rust"][1],
        )
        failed = True

    return failed


def main() -> int:
    setup()

    parser = ArgumentParser()
    parser.add_argument("-j", "--jobs", type=int)
    parser.add_argument("--backend", choices=("cpp", "rust", "both"), default="both")
    parser.add_argument("--rebaseline", action="store_true")

    args = parser.parse_args()

    input_dir = CSS_TOKENIZER_TEST_DIR / "input"
    failed = 0

    css_files = [
        css_file for css_file in sorted(input_dir.iterdir()) if css_file.is_file() and css_file.suffix == ".css"
    ]

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        executables = [
            executor.submit(test, css_file.relative_to(input_dir), args.backend, args.rebaseline)
            for css_file in css_files
        ]

        for executable in as_completed(executables):
            if executable.result():
                failed += 1

    total = len(css_files)
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
