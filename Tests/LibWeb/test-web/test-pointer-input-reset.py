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

ENTRY_EVENTS = "pointerover,mouseover,pointerenter,mouseenter"


def run_case(binary, source, root, name, timeout):
    root = root / name
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
    shutil.copyfile(
        source / "Tests/LibWeb/Text/input/HTML/frame-in-flight/frame-in-flight.js",
        root / "Text/input/frame-in-flight.js",
    )
    target = "<style>html,body{margin:0}#target{width:100px;height:100px;background:green}</style><div id=target></div>"
    if timeout:
        # A test that times out never runs its cleanup, and here the hover lanes still owe the host a hover of their own,
        # with its own pointer position. Only the reset before the next test clears both.
        # The timeout is armed only once the pointer is in, so that a slow setup can't time out before it.
        poison = (
            "await whileLaneHovers(frameTime => {"
            "    internals.injectHoverPointer(50, 50, frameTime + 16);"
            '    console.log("POINTER_RESET_INJECTED");'
            "    internals.setTestTimeout(500);"
            "});"
        )
    else:
        poison = 'internals.mouseMove(50, 50); println("hovered"); done();'
    (root / "Text/input/poison.html").write_text(
        f'<!doctype html>{target}<script src="include.js"></script><script src="frame-in-flight.js"></script>'
        '<script>asyncTest(async done => {console.log("POINTER_RESET_POISON");'
        f"{poison}}});</script>"
    )
    with (root / "Text/expected/poison.txt").open("w", newline="\n") as file:
        file.write("" if timeout else "hovered\n")
    # Text tests precede reference tests. At -j1 these two files use the same view, without relying on directory order.
    (root / "Ref/input/consumer.html").write_text(
        f'<!doctype html><html class="reftest-wait"><link rel="match" href="../expected/green.html">{target}<script>'
        "const events = [];"
        'for (const type of ["pointerover", "mouseover", "pointerenter", "mouseenter"])'
        "    target.addEventListener(type, event => events.push(event.type));"
        "requestAnimationFrame(() => requestAnimationFrame(() => {"
        '    const hovered = document.querySelector(":hover") ? "yes" : "no";'
        "    internals.mouseMove(50, 50);"
        '    console.log(`POINTER_RESET_CONSUMER hovered=${hovered} entry=${events.join(",")}`);'
        "    internals.mouseLeave();"
        '    document.documentElement.classList.remove("reftest-wait");'
        "}));"
        "</script></html>"
    )
    (root / "Ref/expected/green.html").write_text(f"<!doctype html>{target}")
    results = root / "results"
    env = os.environ.copy()
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
    assert process.returncode == int(timeout), f"{name}: unexpected exit status {process.returncode}"
    status = dict(
        line.split(": ", 1) for line in (results / "harness-status.txt").read_text().splitlines() if ": " in line
    )
    assert status["completed"] == "2/2" and status["remaining"] == "0", status
    assert status["pass"] == str(2 - int(timeout)), status
    assert status["timeout"] == str(int(timeout)) and status["fail"] == "0" and status["crashed"] == "0", status
    processes = re.findall(r"ladybird-render\((\d+)\).*POINTER_RESET_(POISON|CONSUMER)", process.stdout)
    assert len(processes) == 2 and processes[0][1] == "POISON" and processes[1][1] == "CONSUMER", processes
    assert processes[0][0] == processes[1][0], f"{name}: WebContent was not reused: {processes}"
    if timeout:
        assert "POINTER_RESET_INJECTED" in process.stdout, f"{name}: the poison timed out before it hovered"
    consumer = re.search(r"POINTER_RESET_CONSUMER hovered=(\w+) entry=([\w,]*)", process.stdout)
    assert consumer, f"{name}: the consumer reported nothing"
    assert consumer.group(1) == "no", f"{name}: the consumer started with an element hovered"
    assert consumer.group(2) == ENTRY_EVENTS, f"{name}: the consumer's first move entered with {consumer.group(2)!r}"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("test_web", type=Path)
    args = parser.parse_args()
    source = Path(os.environ["LADYBIRD_SOURCE_DIR"]).resolve()
    with tempfile.TemporaryDirectory(prefix="test-web-pointer-reset-") as temporary:
        root = Path(temporary)
        run_case(args.test_web.resolve(), source, root, "normal-completion", False)
        # The pointer leaks through the lanes only some of the time, so give a leak several chances to show.
        for attempt in range(5):
            run_case(args.test_web.resolve(), source, root, f"timeout-{attempt}", True)


if __name__ == "__main__":
    main()
