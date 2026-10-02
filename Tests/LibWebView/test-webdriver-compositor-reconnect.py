#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# When the Compositor dies, the browser connects a new one, and the page's next rendering update
# has to bring the new Compositor up to date: its first frame carries the whole display list again,
# not only what changed since the old Compositor's last frame.

import argparse
import base64
import importlib
import os
import shlex
import signal
import subprocess
import tempfile
import time
import urllib.parse

from pathlib import Path

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")

PAGE = "data:text/html," + urllib.parse.quote(
    """<style>
#box { width: 100px; height: 100px; background: blue; }
</style><div id=box></div>"""
)


def descendants_named(root_pid, process_name):
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines()
    children = {}
    for row in rows:
        fields = row.strip().split(None, 2)
        pid, parent = fields[:2]
        command = fields[2] if len(fields) == 3 else ""
        children.setdefault(int(parent), []).append((int(pid), command))
    pending = [root_pid]
    found = []
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            arguments = shlex.split(command)
            if arguments and Path(arguments[0]).name == process_name:
                found.append(pid)
    return found


def execute(port, session, script):
    status, payload, body = webdriver_helpers.request(
        port, "POST", f"/session/{session}/execute/sync", {"script": script, "args": []}
    )
    assert status == 200, body
    return payload["value"]


def take_screenshot(port, session):
    status, payload, body = webdriver_helpers.request(port, "GET", f"/session/{session}/screenshot")
    assert status == 200, body
    png = base64.b64decode(payload["value"])
    assert png.startswith(b"\x89PNG\r\n\x1a\n"), "Screenshot is not a PNG"
    return png


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-compositor-reconnect-") as temporary:
        environment = os.environ.copy()
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        port = webdriver_helpers.unused_port()
        webdriver = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port)],
            env=environment,
        )
        try:
            webdriver_helpers.wait_for_port(port)
            session = webdriver_helpers.create_session(port)
            status, _, body = webdriver_helpers.request(port, "POST", f"/session/{session}/url", {"url": PAGE})
            assert status == 200, body
            first = take_screenshot(port, session)

            compositors = descendants_named(webdriver.pid, "Compositor")
            assert len(compositors) == 1, f"Expected one Compositor, found {len(compositors)}"
            os.kill(compositors[0], signal.SIGKILL)

            deadline = time.monotonic() + webdriver_helpers.EVENT_TIMEOUT_SECONDS
            while True:
                replacements = [pid for pid in descendants_named(webdriver.pid, "Compositor") if pid != compositors[0]]
                if replacements:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError("No Compositor replaced the one that died")
                time.sleep(0.05)

            # The new Compositor paints the page as it was, then as it changes.
            assert take_screenshot(port, session) == first
            execute(port, session, "document.getElementById('box').style.background = 'green';")
            assert take_screenshot(port, session) != first
            webdriver_helpers.request(port, "DELETE", f"/session/{session}")
        finally:
            webdriver.terminate()
            try:
                webdriver.wait(timeout=5)
            except subprocess.TimeoutExpired:
                webdriver.kill()
                webdriver.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
