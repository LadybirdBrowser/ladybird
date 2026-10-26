#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import runpy
import subprocess
import tempfile

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


# A tab opened from a WebUI page starts in that page's process, so a second page of the same WebUI there must not take
# the first page's channel to the UI process.
def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory() as directory:
        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
            session = helpers["create_session"](port)

            def command(path, body):
                status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def round_trip():
                return command(
                    "execute/async",
                    {
                        "script": """
                        const done = arguments[0];
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (event.detail.name !== 'loadSettings') return;
                            document.removeEventListener('WebUIMessage', listener);
                            done(true);
                        });
                        ladybird.sendMessage('loadCurrentSettings');
                    """,
                        "args": [],
                    },
                )

            command("timeouts", {"script": 5000})
            command("url", {"url": "about:settings"})
            assert round_trip()
            status, payload, raw = helpers["request"](port, "GET", f"/session/{session}/window")
            assert status == 200, raw
            first_window = payload["value"]

            second_window = command("window/new", {"type": "tab"})["handle"]
            command("window", {"handle": second_window})
            command("url", {"url": "about:settings"})
            assert round_trip()

            command("window", {"handle": first_window})
            assert round_trip(), "The first page lost its WebUI channel"
            print("PASS: two WebUI pages in one process each keep their channel")
        finally:
            try:
                if session is not None:
                    helpers["request"](port, "DELETE", f"/session/{session}")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
