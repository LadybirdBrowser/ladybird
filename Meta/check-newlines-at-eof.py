#!/usr/bin/env python3

import os
import re
import subprocess
import sys

RE_RELEVANT_FILE_EXTENSION = re.compile("\\.(cpp|h|mm|swift|gml|html|js|css|sh|py|json|txt|cmake|gn|gni)$")


def should_check_file(filename):
    if not RE_RELEVANT_FILE_EXTENSION.search(filename):
        return False
    if filename.startswith("Tests/LibWeb/Crash/"):
        return False
    if filename.startswith("Tests/LibWeb/Layout/"):
        return False
    if filename.startswith("Tests/LibWeb/Ref/"):
        return False
    if filename.startswith("Tests/LibWeb/Screenshot/"):
        return False
    if filename.startswith("Tests/LibWeb/Text/"):
        return False
    if filename.startswith("Meta/CMake/vcpkg/overlay-ports/"):
        return False
    if filename.endswith(".txt"):
        return "CMake" in filename
    return True


def find_files_here_or_argv():
    if len(sys.argv) > 1:
        raw_list = sys.argv[1:]
    else:
        process = subprocess.run(["git", "ls-files"], check=True, capture_output=True)
        raw_list = process.stdout.decode().strip("\n").split("\n")

    return filter(should_check_file, raw_list)


def run():
    """Check files checked in to git for trailing newlines at end of file."""
    no_newline_at_eof_errors = []
    whitespace_at_eof_errors = []

    for filename in find_files_here_or_argv():
        with open(filename, "r", newline="") as f:
            content = f.read()

        if not content.endswith("\n"):
            no_newline_at_eof_errors.append(filename)
            continue

        stripped = content.rstrip()
        trailing_whitespace = len(content) - len(stripped)
        if trailing_whitespace > 1:
            whitespace_at_eof_errors.append(filename)

    if no_newline_at_eof_errors:
        print("Files with no newline at the end:", " ".join(no_newline_at_eof_errors))
    if whitespace_at_eof_errors:
        print("Files that have whitespace at the end:", " ".join(whitespace_at_eof_errors))

    if no_newline_at_eof_errors or whitespace_at_eof_errors:
        sys.exit(1)


if __name__ == "__main__":
    os.chdir(os.path.dirname(__file__) + "/..")
    run()
