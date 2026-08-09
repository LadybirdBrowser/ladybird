#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import os
import runpy
import tempfile
import unittest

from pathlib import Path

SCRIPT = Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta" / "analyze-xctrace-time-profile.py"
MODULE = runpy.run_path(str(SCRIPT), run_name="analyze_xctrace_time_profile")


class TestXctraceTimeProfile(unittest.TestCase):
    def test_resolves_referenced_weights_frames_and_backtraces(self):
        exported_table = """<?xml version="1.0"?>
<trace-query-result><node>
<row><weight id="1">1000000</weight><tagged-backtrace id="2"><backtrace id="3">
<frame id="4" name="leaf"/><frame id="5" name="caller"/>
</backtrace></tagged-backtrace></row>
<row><weight ref="1"/><tagged-backtrace ref="2"/></row>
<row><weight id="6">2000000</weight><tagged-backtrace id="7"><backtrace id="8">
<frame ref="4"/><frame id="9" name="outer"/>
</backtrace></tagged-backtrace></row>
</node></trace-query-result>
"""
        with tempfile.NamedTemporaryFile("w", suffix=".xml") as export:
            export.write(exported_table)
            export.flush()
            sample_count, weights = MODULE["inclusive_weights"](export.name)

        self.assertEqual(sample_count, 3)
        self.assertEqual(weights, {"leaf": 4_000_000, "caller": 2_000_000, "outer": 2_000_000})


if __name__ == "__main__":
    unittest.main()
