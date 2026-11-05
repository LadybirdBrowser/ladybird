#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("test_web", type=Path)
    args = parser.parse_args()
    source = Path(os.environ["LADYBIRD_SOURCE_DIR"]).resolve()

    with tempfile.TemporaryDirectory(prefix="test-web-history-rate-limit-reset-") as temporary:
        root = Path(temporary)
        for directory in ("Accessibility", "Layout", "Text", "Ref", "Screenshot"):
            for kind in ("input", "expected"):
                (root / directory / kind).mkdir(parents=True)
        (root / "Crash").mkdir()
        (root / "TestConfig.ini").write_text("")
        for file in ("Fixtures/http-test-server.py", "test-web/results-index.html"):
            (root / file).parent.mkdir()
            shutil.copyfile(source / "Tests/LibWeb" / file, root / file)
        (root / "Text/input/wpt-import").mkdir()
        shutil.copyfile(source / "Tests/LibWeb/Text/input/include.js", root / "Text/input/include.js")
        # More history updates than a navigable may make in one rate-limit window. The UI process applies the accepted
        # ones one at a time, after the page's script has finished, so wait for that before ending the test: Otherwise
        # the next test's load waits behind them, and on a slow runner can time out before they're done.
        (root / "Text/input/flood.html").write_text(
            '<!doctype html><script src="include.js"></script><script>asyncTest(async done => {'
            'console.log("HISTORY_RESET_FLOOD");'
            'for (let i = 0; i < 250; ++i) history.pushState(null, "", "#" + i);'
            "await internals.flushSessionHistoryTraversalQueue();"
            'println("flooded"); done();'
            "});</script>"
        )
        with (root / "Text/expected/flood.txt").open("w", newline="\n") as file:
            file.write("flooded\n")
        # Text tests precede reference tests. At -j1 these two files use the same view, without relying on directory
        # order. The page turns green only if its own history update goes through.
        (root / "Ref/input/consumer.html").write_text(
            '<!doctype html><html class="reftest-wait">'
            '<link rel="match" href="../expected/green.html">'
            "<style>html{background:red}body{margin:0}</style>"
            '<script>console.log("HISTORY_RESET_CONSUMER");'
            "const length = history.length;"
            'history.pushState(null, "", "#consumer");'
            'if (history.length === length + 1) document.documentElement.style.background = "green";'
            'requestAnimationFrame(() => document.documentElement.classList.remove("reftest-wait"));'
            "</script></html>"
        )
        (root / "Ref/expected/green.html").write_text(
            "<!doctype html><style>html,body{margin:0;background:green}</style>"
        )

        results = root / "results"
        env = os.environ.copy()
        # The echo server imports its vendored tools even though these fixtures use only file URLs. Keep those tools
        # outside the private input tree so test discovery cannot collect their own HTML fixtures.
        wpt = source / "Tests/LibWeb/Text/input/wpt-import"
        env["PYTHONPATH"] = os.pathsep.join((str(wpt), str(wpt / "_wpttools")))
        process = subprocess.run(
            [
                str(args.test_web.resolve()),
                "--test-path",
                str(root),
                "-j1",
                "-v",
                "-v",
                "--per-test-timeout",
                "30",
                "--python-executable",
                sys.executable,
                "--results-dir",
                str(results),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=100,
        )
        print(process.stdout, flush=True)
        assert process.returncode == 0, f"unexpected exit status {process.returncode}"
        status = dict(
            line.split(": ", 1) for line in (results / "harness-status.txt").read_text().splitlines() if ": " in line
        )
        assert status["completed"] == "2/2" and status["pass"] == "2", status
        processes = re.findall(r"WebContent\((\d+)\).*HISTORY_RESET_(FLOOD|CONSUMER)", process.stdout)
        assert len(processes) == 2 and processes[0][1] == "FLOOD" and processes[1][1] == "CONSUMER", processes
        assert processes[0][0] == processes[1][0], f"WebContent was not reused: {processes}"


if __name__ == "__main__":
    main()
