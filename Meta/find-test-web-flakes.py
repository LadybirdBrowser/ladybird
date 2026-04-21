#!/usr/bin/env python3

import argparse
import subprocess
import sys

from pathlib import Path

VALID_SUFFIXES = (".htm", ".html", ".svg", ".xhtml", ".xht")
SUITES = (
    ("Layout", "input"),
    ("Text", "input"),
    ("Ref", "input"),
    ("Screenshot", "input"),
    ("Crash", None),
)


def load_skipped_tests(test_root: Path) -> set[str]:
    skipped = set()
    in_skipped_section = False

    for raw_line in test_root.joinpath("TestConfig.ini").read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("[") and line.endswith("]"):
            in_skipped_section = line == "[Skipped]"
            continue
        if not in_skipped_section:
            continue

        path = test_root / line
        if line.endswith("/"):
            for candidate in sorted(path.rglob("*")):
                if candidate.is_file() and candidate.name.endswith(VALID_SUFFIXES):
                    skipped.add(candidate.relative_to(test_root).as_posix())
            continue

        skipped.add(line)

    return skipped


def iter_tests(test_root: Path, filters: list[str]) -> str:
    for suite, input_dir in SUITES:
        root = test_root / suite
        if input_dir is not None:
            root /= input_dir

        for path in sorted(root.rglob("*")):
            if not path.is_file():
                continue

            relative_path = path.relative_to(test_root).as_posix()
            if filters and not any(filter_text in relative_path for filter_text in filters):
                continue
            if not path.name.endswith(VALID_SUFFIXES):
                continue

            yield relative_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="""
            Find flaky tests in Tests/LibWeb. This performs a single `Build/release/bin/test-web
            --repeat N` run for each matched test. It logs failures to ./flakes.log. The log format
            is "<test-path> <exit-code>" where exit code is usually <failure-count> % 256. Zero
            exit codes (including sneaky failure counts that are multiples of 256) are not logged.
        """
    )

    parser.add_argument("-f", dest="filters", action="append", default=[], help="Substring filter on test path")
    parser.add_argument("-n", dest="repeat_count", type=int, default=512, help="Repeat count for individual tests")
    args = parser.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    if subprocess.run([str(repo_root / "Meta" / "ladybird.py"), "build", "test-web"], cwd=repo_root).returncode != 0:
        print("failed to build test-web", file=sys.stderr)
        return 1

    test_web = repo_root / "Build" / "release" / "bin" / "test-web"
    if not test_web.is_file():
        test_web = repo_root / "Build" / "release" / "bin" / "Ladybird.app" / "Contents" / "MacOS" / "test-web"
    if not test_web.is_file():
        print("missing test-web binary in Build/release/bin or Ladybird.app/Contents/MacOS", file=sys.stderr)
        return 1

    test_root = repo_root / "Tests" / "LibWeb"
    skipped = load_skipped_tests(test_root)

    with repo_root.joinpath("flakes.log").open("w", encoding="utf-8") as log_file:
        for relative_path in iter_tests(test_root, args.filters):
            if relative_path in skipped:
                continue
            result = subprocess.run(
                [str(test_web), "--repeat", str(args.repeat_count), "-f", relative_path],
                cwd=repo_root,
            )
            if result.returncode == 0:
                continue

            log_file.write(f"{relative_path} {result.returncode}\n")
            log_file.flush()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
