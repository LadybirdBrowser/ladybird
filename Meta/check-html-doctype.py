#!/usr/bin/env python3

import os
import re
import subprocess
import sys

# FIXME: Include Layout tests in general.
#        They are currently deferred to a later PR to make review easier for now.
RE_RELEVANT_FILE = re.compile('^Tests/LibWeb/(Ref|Screenshot|Text)/(.(?!wpt-import/))*\\.html$')
RE_DOCTYPE = re.compile('^<!doctype .*>', re.IGNORECASE)


def should_check_file(filename):
    return RE_RELEVANT_FILE.match(filename) is not None


def find_files_here_or_argv():
    if len(sys.argv) > 1:
        raw_list = sys.argv[1:]
    else:
        process = subprocess.run(["git", "ls-files"], check=True, capture_output=True)
        raw_list = process.stdout.decode().strip('\n').split('\n')

    return filter(should_check_file, raw_list)


def run():
    files_with_missing_doctypes = []

    for filename in find_files_here_or_argv():
        with open(filename, 'r') as file:
            if not RE_DOCTYPE.search(file.readline()):
                files_with_missing_doctypes.append(filename)

    if files_with_missing_doctypes:
        print('The following HTML files should include a doctype declaration at the start of the file but don\'t:\n' +
              'You should add <!DOCTYPE html> to the very beginning of these files, except if they absolutely need ' +
              'to run in quirks mode. In that case, you can clearly indicate so with a bogus doctype that says ' +
              '"quirks" instead of "html".\n',
              ' '.join(files_with_missing_doctypes))
        sys.exit(1)


if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__) + "/..")
    run()
