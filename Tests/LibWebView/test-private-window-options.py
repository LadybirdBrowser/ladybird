#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import subprocess
import sys

if __name__ == "__main__":
    browser = [sys.argv[1], "--temporary-profile", "--headless=text"]

    # --private requires --new-window.
    result = subprocess.run([*browser, "--private"], stderr=subprocess.PIPE, timeout=15)
    assert result.returncode != 0 and b"--private requires --new-window" in result.stderr, result

    # Headless mode ignores the private-window request and prints the page text.
    output = subprocess.check_output(
        [*browser, "--new-window", "--private", "data:text/html,headless"], timeout=15, text=True
    )
    assert output == "headless\n", output
