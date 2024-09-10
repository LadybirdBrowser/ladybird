#!/usr/bin/env python3

import argparse
import os
import pathlib
import re
import subprocess
import sys

script_name = pathlib.Path(__file__).resolve().name

lines_to_skip = re.compile(
    r"^($| *//|\};|#import |.+ includes .+|\[[^\]]+\]"
    r"|interface |dictionary |enum |namespace |typedef |callback )"
)

parser = argparse.ArgumentParser()
parser.add_argument("--overwrite-inplace", action=argparse.BooleanOptionalAction)
parser.add_argument('filenames', nargs='*')
args = parser.parse_args()


def find_files_here_or_argv():
    if args.filenames:
        raw_list = args.filenames
    else:
        process = subprocess.run(["git", "ls-files"], check=True, capture_output=True)
        raw_list = process.stdout.decode().strip("\n").split("\n")
    return filter(lambda filename: filename.endswith(".idl"), raw_list)


def run():
    """Lint WebIDL files checked into git for four leading spaces on each line."""
    files_without_four_leading_spaces = set()
    did_fail = False
    for filename in find_files_here_or_argv():
        lines = []
        with open(filename, "r") as f:
            for line_number, line in enumerate(f, start=1):
                if lines_to_skip.match(line):
                    lines.append(line)
                    continue
                if not line.startswith("    "):
                    if args.overwrite_inplace:
                        line = "    " + line.lstrip()
                        lines.append(line)
                        continue
                    did_fail = True
                    files_without_four_leading_spaces.add(filename)
                    print(
                        f"{filename}:{line_number} error: Line does not start with four spaces:{line.rstrip()}")
                lines.append(line)
        if args.overwrite_inplace:
            with open(filename, "w") as f:
                f.writelines(lines)

    if files_without_four_leading_spaces:
        print("\nWebIDL files that have lines without four leading spaces:",
              " ".join(files_without_four_leading_spaces))
        if not args.overwrite_inplace:
            print(
                f"\nTo fix the WebIDL files in place, run: ./Meta/{script_name} --overwrite-inplace")
    if did_fail:
        sys.exit(1)


if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__) + "/..")
    run()
