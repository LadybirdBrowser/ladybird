#!/usr/bin/env python3

# Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import re
import sys


def error(message: str, commit_message: str) -> None:
    print(f"\033[0;31m{message}:\033[0m", file=sys.stderr)
    print(commit_message, file=sys.stderr)
    sys.exit(1)


def check_commit_message(commit_file: str) -> None:
    line_number = 0
    lines = []

    # See newline parameter at https://docs.python.org/3/library/functions.html#open. Setting to '' ensures
    # CRLFs are not automatically translated to LFs. Otherwise, we'd need to read the file in binary
    # mode and decode to UTF-8
    with open(commit_file, "r", newline="", encoding="utf-8") as f:
        commit_message = f.read()

        # Check for CRLF line breaks
        if "\x0d" in commit_message:
            error(
                "Commit message contains CRLF line breaks (only unix-style LF linebreaks are allowed)", commit_message
            )
        f.seek(0)
        lines = f.readlines()

    if not lines:
        error("Empty commit message", commit_message)

    for line in lines:
        # Remove trailing newline for processing
        line = line.rstrip("\n\r")

        # Break on git cut line, used by git commit --verbose
        if line == "# ------------------------ >8 ------------------------":
            break

        # Ignore comment lines
        if re.match(r"^#.*", line):
            continue

        # Ignore overlong 'fixup!' commit descriptions
        if re.match(r"^fixup! .*", line):
            continue

        line_number += 1
        line_length = len(line)

        if line_number == 2 and line_length != 0:
            error("Empty line between commit title and body is missing", commit_message)

        merge_commit_pattern = r"^Merge branch"
        if line_number == 1 and re.search(merge_commit_pattern, line):
            error("Commit is a git merge commit, use the rebase command instead", commit_message)

        category_pattern = r'^(Revert "|\S+: )'
        if line_number == 1 and not re.search(category_pattern, line):
            error(
                "Missing category in commit title (if this is a fix up of a previous commit, it should be squashed)",
                commit_message,
            )

        title_case_pattern = r"^\S.*?: [A-Z0-9]"
        if line_number == 1 and not re.search(title_case_pattern, line):
            error("First word of commit after the subsystem is not capitalized", commit_message)

        if line_number == 1 and line.endswith("."):
            error("Commit title ends in a period", commit_message)

        url_pattern = r"([a-z]+:\/\/)?(([a-zA-Z0-9_]|-)+\.)+[a-z]{2,}(:\d+)?([a-zA-Z_0-9@:%\+.~\?&\/=]|-)+"
        if line_length > 72 and not re.search(url_pattern, line):
            error("Commit message lines are too long (maximum allowed is 72 characters)", commit_message)

        if line.startswith("Signed-off-by: "):
            error("Commit body contains a Signed-off-by tag", commit_message)


def main():
    parser = argparse.ArgumentParser(description="Check commit message formatting")
    parser.add_argument("commit_file", help="File containing the commit message")

    args = parser.parse_args()

    check_commit_message(args.commit_file)


if __name__ == "__main__":
    main()
