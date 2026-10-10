#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""Sharing the machine with work that needs it idle, like benchmarks.

While one of the pause files exists, the test tools start nothing new. LIBJS_JIT_TEST_PAUSE_FILES lists them,
separated by colons.
"""

import os
import time

from pathlib import Path


def pause_files():
    return [Path(path) for path in os.environ.get("LIBJS_JIT_TEST_PAUSE_FILES", "").split(":") if path]


def wait_until_machine_is_free():
    """Returns once none of the pause files exists."""
    while any(path.exists() for path in pause_files()):
        time.sleep(20)
