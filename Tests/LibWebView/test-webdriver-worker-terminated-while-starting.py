#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import os
import subprocess
import tempfile
import time

from pathlib import Path

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")

# Terminate a worker at each of these delays after constructing it, to catch its thread at every stage of starting up.
TERMINATE_DELAYS_MS = list(range(0, 600, 25))

# How long a crashed worker's report gets to show up.
CRASH_REPORT_GRACE_SECONDS = 2


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-worker-terminated-") as temporary:
        environment = os.environ.copy()
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        profile = Path(temporary) / "profile"
        webdriver, port = webdriver_helpers.launch_webdriver(
            webdriver_binary, "--headless", f"--profile-path={profile}", env=environment
        )
        try:
            session = webdriver_helpers.create_session(port)
            status, _, body = webdriver_helpers.request(
                port, "POST", f"/session/{session}/url", {"url": "data:text/html,<title>Workers</title>"}
            )
            assert status == 200, body

            # Each worker's thread is started before its constructor returns, and is still starting up when
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

            # A worker that crashes on its way out takes the WebContent process that hosts it along.
            time.sleep(CRASH_REPORT_GRACE_SECONDS)
            reports = list((profile / "data/CrashReports").glob("*-WebContent-*.txt"))
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
