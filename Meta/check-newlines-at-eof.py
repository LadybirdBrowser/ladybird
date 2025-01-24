#!/usr/bin/env python3

import os
import re
import subprocess
import sys


RE_RELEVANT_FILE_EXTENSION = re.compile('\\.(cpp|h|mm|swift|gml|html|js|css|sh|py|json|txt|cmake|gn|gni)$')


def should_check_file(filename):
    if not RE_RELEVANT_FILE_EXTENSION.search(filename):
        return False
    if filename.startswith('Tests/LibWeb/Layout/'):
        return False
    if filename.startswith('Tests/LibWeb/Ref/'):
        return False
    if filename.startswith('Tests/LibWeb/Text/'):
        return False
    if filename.endswith('.txt'):
        return 'CMake' in filename
    return True


def find_files_here_or_argv():
    if len(sys.argv) > 1:
        raw_list = sys.argv[1:]
    else:
        process = subprocess.run(["git", "ls-files"], check=True, capture_output=True)
        raw_list = process.stdout.decode().strip('\n').split('\n')

    return filter(should_check_file, raw_list)


def run():
    """Check files checked in to git for trailing newlines at end of file."""
    no_newline_at_eof_errors = []
    blank_lines_at_eof_errors = []

    did_fail = False
    for filename in find_files_here_or_argv():
        with open(filename, "r") as f:
            f.seek(0, os.SEEK_END)

            f.seek(f.tell() - 1, os.SEEK_SET)
            if f.read(1) != '\n':
                did_fail = True
                no_newline_at_eof_errors.append(filename)
                continue

            while True:
                f.seek(f.tell() - 2, os.SEEK_SET)
                char = f.read(1)
                if not char.isspace():
                    break
                if char == '\n':
                    did_fail = True
                    blank_lines_at_eof_errors.append(filename)
                    break

    if no_newline_at_eof_errors:
        print("Files with no newline at the end:", " ".join(no_newline_at_eof_errors))
    if blank_lines_at_eof_errors:
        print("Files that have blank lines at the end:", " ".join(blank_lines_at_eof_errors))

    if did_fail:
        sys.exit(1)


if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__) + "/..")
    run()
