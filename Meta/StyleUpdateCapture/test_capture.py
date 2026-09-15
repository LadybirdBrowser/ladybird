#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Arithmetic, portability and comparison checks; no browser or benchmark is launched."""

import argparse
import contextlib
import copy
import io
import json
import os
import runpy
import sys
import tempfile
import unittest

from pathlib import Path
from types import SimpleNamespace

capture = SimpleNamespace(**runpy.run_path(str(Path(__file__).parents[1] / "style-update-capture.py")))


def sample(amount=1):
    before = {
        "rust": {
            "transactionMicroseconds": 20,
            "commitMicroseconds": 2,
            "routingPlanningMicroseconds": 3,
            "matchingCascadeMicroseconds": 4,
            "computationPublicationMicroseconds": 5,
            "emitMicroseconds": 1,
            "transactionRemainderMicroseconds": 5,
            "reachedStyleNodes": 10,
        },
        "cpp": {
            "styleUpdateMicroseconds": 40,
            "styleUpdateSubmissionMicroseconds": 5,
            "styleUpdateBridgeMicroseconds": 20,
            "styleUpdateApplyMicroseconds": 10,
            "styleUpdateRemainderMicroseconds": 5,
        },
    }
    after = {lane: {key: value * (amount + 1) for key, value in fields.items()} for lane, fields in before.items()}
    return {
        "name": "class-add",
        "kind": "mutation",
        "before": before,
        "after": after,
        "cpuMicroseconds": 100,
        "rssBytes": 4096,
        "mutationMicroseconds": 2,
    }


def document():
    return {
        "version": 1,
        "scenario": "connected",
        "seed": 1,
        "elementCount": 20000,
        "fixtureSha256": "fixture",
        "stylebenchUrl": None,
        "boundaries": {"unmeasured": ["style-only CPU"]},
        "runs": [{"config": "A", "run": 1, "samples": [capture.normalize(sample())]}],
    }


class CaptureTests(unittest.TestCase):
    def test_nested_clocks_are_kept_separate(self):
        row = capture.normalize(sample())
        self.assertEqual(row["metrics"]["rust.transactionMicroseconds"], 20)
        self.assertEqual(row["metrics"]["cpp.styleUpdateMicroseconds"], 40)
        self.assertEqual(row["reachedNodes"], 10)

    def test_bad_partition_is_rejected(self):
        row = sample()
        row["after"]["rust"]["commitMicroseconds"] += 1
        with self.assertRaisesRegex(RuntimeError, "Non-additive rust"):
            capture.normalize(row)

    def test_decreasing_clocks_are_rejected(self):
        row = sample(-1)
        with self.assertRaisesRegex(RuntimeError, "Non-additive"):
            capture.normalize(row)

    def test_process_replacement_does_not_subtract_unrelated_cpu(self):
        before = [{"pid": 1, "startedAt": "Mon Sep 14 09:00:00 2026", "cpuMicroseconds": 10, "rssBytes": 4096}]
        after = [{"pid": 1, "startedAt": "Mon Sep 14 09:00:01 2026", "cpuMicroseconds": 3, "rssBytes": 8192}]
        self.assertIsNone(capture.resource_delta(before, after)["cpuMicroseconds"])
        self.assertEqual(capture.resource_delta(before, after)["rssBytes"], 8192)
        self.assertIsNone(capture.resource_delta([], [])["rssBytes"])

    def test_both_platforms_cpu_time_cells_parse(self):
        # Linux prints whole seconds; macOS prints hundredths and drops leading units.
        self.assertEqual(capture.cpu_microseconds("00:02:27"), 147_000_000)
        self.assertEqual(capture.cpu_microseconds("2-01:00:00"), 176_400_000_000)
        self.assertEqual(capture.cpu_microseconds("12:07.35"), 727_350_000)
        self.assertEqual(capture.cpu_microseconds("0.03"), 30_000)

    def test_process_table_reads_this_platform(self):
        rows = capture.process_table()
        if not rows:
            self.skipTest("no usable `ps` on this platform")
        self.assertIn(os.getpid(), [row["pid"] for row in rows])
        self.assertIn(os.getpgrp(), [row["group"] for row in rows])
        self.assertTrue(all(row["rssBytes"] >= 0 and row["cpuMicroseconds"] >= 0 for row in rows))

    def test_library_hashing_follows_the_platform(self):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            (build / "lib").mkdir()
            for name in ["liblagom-web.so", "liblagom-web.dylib", "liblagom-websocket.so"]:
                (build / "lib" / name).write_text(name)
            expected = "liblagom-web.dylib" if sys.platform == "darwin" else "liblagom-web.so"
            self.assertEqual([path.name for path in capture.web_libraries(build)], [expected])
            (build / "lib" / expected).unlink()
            found = [path.name for path in capture.web_libraries(build)]
            self.assertNotIn("liblagom-websocket.so", found)
            self.assertEqual(capture.web_libraries(Path(directory) / "absent"), [])

    def compare(self, left, right=None, a=None, b=None):
        with tempfile.TemporaryDirectory() as directory:
            first = Path(directory) / "a.json"
            second = Path(directory) / "b.json" if right is not None else first
            first.write_text(json.dumps(left))
            if right is not None:
                second.write_text(json.dumps(right))
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                capture.compare(argparse.Namespace(before=first, after=second, a=a, b=b, summary=False))
            return output.getvalue()

    def test_medians_missing_metrics_and_unmatched_intervals(self):
        left = document()
        right = copy.deepcopy(left)
        right["runs"].append({"config": "A", "run": 2, "samples": [capture.normalize(sample(3))]})
        unmatched = capture.normalize(sample())
        unmatched["name"] = "extra"
        right["runs"][0]["samples"].append(unmatched)
        output = self.compare(left, right)
        self.assertIn("40.00/40.00/40.00 -> 80.00/120.00/120.00; +100.00%", output)
        self.assertIn("observationMicroseconds: UNMEASURED", output)
        self.assertIn("UNMATCHED interval ('mutation', 'extra')", output)
        self.assertIn("UNMATCHED run counts", output)

    def test_zero_is_not_a_percentage_denominator(self):
        data = document()
        data["runs"][0]["samples"] = [capture.normalize(sample(0))]
        self.assertIn("not applicable (A median is zero)", self.compare(data))

    def test_paired_configs_and_incompatible_scenarios(self):
        data = document()
        data["runs"].append({"config": "B", "run": 1, "samples": [capture.normalize(sample(2))]})
        self.assertIn("Paired B-A median: +40.00; 1 pairs", self.compare(data, a="A", b="B"))
        other = copy.deepcopy(data)
        other["scenario"] = "detached"
        self.assertIn("UNMATCHED capture scenario", self.compare(data, other, a="A", b="B"))


if __name__ == "__main__":
    unittest.main()
