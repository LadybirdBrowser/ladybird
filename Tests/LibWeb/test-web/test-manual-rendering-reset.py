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


def run_case(binary, source, root, name, manual, timeout):
    root = root / name
    for directory in ("Accessibility", "Layout", "Text", "Ref", "Screenshot"):
        for kind in ("input", "expected"):
            (root / directory / kind).mkdir(parents=True)
    (root / "Crash").mkdir()
    (root / "TestConfig.ini").write_text("")
    (root / "Fixtures").symlink_to(source / "Tests/LibWeb/Fixtures", target_is_directory=True)
    (root / "test-web").symlink_to(source / "Tests/LibWeb/test-web", target_is_directory=True)
    (root / "Text/input/wpt-import").mkdir()
    shutil.copyfile(source / "Tests/LibWeb/Text/input/include.js", root / "Text/input/include.js")
    finish = "internals.setTestTimeout(100);" if timeout else 'println("armed"); done();'
    (root / "Text/input/poison.html").write_text(
        '<!doctype html><script src="include.js"></script><script>asyncTest(done => {'
        'console.log("MANUAL_RESET_POISON");'
        f"internals.setManualRenderingOpportunities({str(manual).lower()}); {finish}"
        "});</script>"
    )
    (root / "Text/expected/poison.txt").write_text("" if timeout else "armed\n")
    # Text tests precede reference tests. At -j1 these two files use the same view, without relying on directory order.
    (root / "Ref/input/consumer.html").write_text(
        '<!doctype html><html class="reftest-wait">'
        '<link rel="match" href="../expected/green.html">'
        "<style>html,body{margin:0;background:green}</style>"
        '<script>console.log("MANUAL_RESET_CONSUMER");'
        'requestAnimationFrame(() => document.documentElement.classList.remove("reftest-wait"));'
        "</script></html>"
    )
    (root / "Ref/expected/green.html").write_text("<!doctype html><style>html,body{margin:0;background:green}</style>")
    results = root / "results"
    env = os.environ.copy()
    env["LADYBIRD_SOURCE_DIR"] = str(source)
    # The echo server imports its vendored tools even though these fixtures use only file URLs. Keep those tools
    # outside the private input tree so test discovery cannot collect their own HTML fixtures.
    wpt = source / "Tests/LibWeb/Text/input/wpt-import"
    env["PYTHONPATH"] = os.pathsep.join((str(wpt), str(wpt / "_wpttools")))
    process = subprocess.run(
        [
            str(binary),
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
    print(f"{name}:\n{process.stdout}", flush=True)
    # A deadline deliberately completes only the first fixture in the timeout case. It never synchronizes a frame.
    assert process.returncode == int(timeout), f"{name}: unexpected exit status {process.returncode}"
    status = dict(
        line.split(": ", 1) for line in (results / "harness-status.txt").read_text().splitlines() if ": " in line
    )
    assert status["completed"] == "2/2" and status["remaining"] == "0", status
    assert status["pass"] == str(2 - int(timeout)), status
    assert status["timeout"] == str(int(timeout)) and status["fail"] == "0" and status["crashed"] == "0", status
    processes = re.findall(r"WebContent\((\d+)\).*MANUAL_RESET_(POISON|CONSUMER)", process.stdout)
    assert len(processes) == 2 and processes[0][1] == "POISON" and processes[1][1] == "CONSUMER", processes
    assert processes[0][0] == processes[1][0], f"{name}: WebContent was not reused: {processes}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("test_web", type=Path)
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="test-web-manual-reset-") as temporary:
        root = Path(temporary)
        run_case(args.test_web.resolve(), args.source.resolve(), root, "control", False, False)
        run_case(args.test_web.resolve(), args.source.resolve(), root, "normal-completion", True, False)
        run_case(args.test_web.resolve(), args.source.resolve(), root, "timeout", True, True)


if __name__ == "__main__":
    main()
