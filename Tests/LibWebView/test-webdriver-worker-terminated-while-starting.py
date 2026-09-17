#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import os
import shlex
import subprocess
import tempfile
import time

from pathlib import Path

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")

# Terminate a worker at each of these delays after constructing it, to catch its process at every stage of starting up.
TERMINATE_DELAYS_MS = list(range(0, 600, 25))

# How long a crashed worker's report gets to show up, once every worker process is gone.
CRASH_REPORT_GRACE_SECONDS = 2


def descendants_named(root_pid, process_name):
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines()
    children = {}
    for row in rows:
        fields = row.strip().split(None, 2)
        pid, parent = fields[:2]
        command = fields[2] if len(fields) == 3 else ""
        children.setdefault(int(parent), []).append((int(pid), command))
    pending = [root_pid]
    descendants = []
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            arguments = shlex.split(command)
            if arguments and Path(arguments[0]).name == process_name:
                descendants.append(pid)
    return descendants


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-worker-terminated-") as temporary:
        environment = os.environ.copy()
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        port = webdriver_helpers.unused_port()
        webdriver = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port)], env=environment
        )
        try:
            webdriver_helpers.wait_for_port(port)
            session = webdriver_helpers.create_session(port)
            status, _, body = webdriver_helpers.request(
                port, "POST", f"/session/{session}/url", {"url": "data:text/html,<title>Workers</title>"}
            )
            assert status == 200, body

            # Each worker's process is launched before its constructor returns, and is still starting up when
            # terminate() tears the worker down.
            status, payload, body = webdriver_helpers.request(
                port,
                "POST",
                f"/session/{session}/execute/async",
                {
                    "script": """
                    const [delays, done] = arguments;
                    const url = URL.createObjectURL(new Blob(["onmessage = () => {};"], {type: 'text/javascript'}));
                    let remaining = delays.length;
                    for (const delay of delays) {
                        const worker = new Worker(url);
                        const terminate = () => {
                            worker.terminate();
                            if (--remaining === 0)
                                done();
                        };
                        if (delay === 0)
                            terminate();
                        else
                            setTimeout(terminate, delay);
                    }
                """,
                    "args": [TERMINATE_DELAYS_MS],
                },
            )
            assert status == 200 and payload == {"value": None}, body

            deadline = time.monotonic() + webdriver_helpers.EVENT_TIMEOUT_SECONDS
            while descendants_named(webdriver.pid, "WebWorker"):
                if time.monotonic() >= deadline:
                    raise AssertionError("A terminated worker's process never exited")
                time.sleep(0.05)

            time.sleep(CRASH_REPORT_GRACE_SECONDS)
            reports = list((Path(temporary) / "data/Ladybird/CrashReports").glob("*-WebWorker-*.txt"))
            assert not reports, reports[0].read_text()

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
