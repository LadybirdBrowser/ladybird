#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
DevToolsClient = devtools["DevToolsClient"]
helpers = devtools["helpers"]
stop = devtools["stop"]
wait_until = devtools["wait_until"]
TIMEOUT_SECONDS = helpers["EVENT_TIMEOUT_SECONDS"]

USER_AGENT_PRESET = "Safari macOS Desktop"

# The User-Agent of the last request each path received, by path.
requests_seen = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        requests_seen[self.path] = self.headers.get("User-Agent", "")
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b"<!doctype html><title>/page</title>")

    def log_message(self, format, *args):
        pass


def launch(ladybird_binary, profile, url):
    # Like the DevTools tests' launch(), with the User-Agent preset and the internals object the test needs.
    deadline = time.monotonic() + TIMEOUT_SECONDS
    for _ in range(5):
        if time.monotonic() >= deadline:
            break
        devtools_port = helpers["unused_port"]()
        process = subprocess.Popen(
            [
                ladybird_binary,
                "--headless=manual",
                "--expose-internals-object",
                f"--profile-path={profile}",
                f"--devtools={devtools_port}",
                f"--user-agent-preset={USER_AGENT_PRESET}",
                url,
            ],
            stderr=subprocess.PIPE,
            text=True,
        )
        bind_failed = threading.Event()

        def forward_stderr(stderr=process.stderr, bind_failed=bind_failed):
            assert stderr is not None
            for line in stderr:
                if line.startswith("Unable to launch devtools server"):
                    bind_failed.set()
                sys.stderr.write(line)

        threading.Thread(target=forward_stderr, daemon=True).start()

        while not bind_failed.is_set() and process.poll() is None and time.monotonic() < deadline:
            try:
                attempt_deadline = min(deadline, time.monotonic() + 1)
                return process, DevToolsClient(devtools_port, deadline=attempt_deadline)
            except OSError:
                pass
        stop(process)
    raise AssertionError("The DevTools server didn't start")


def test_force_ladybird_overrides_the_preset(ladybird_binary, profile, server):
    base = f"http://127.0.0.1:{server.server_port}"
    process, client = launch(ladybird_binary, profile, f"{base}/page")
    try:
        wait_until("the tab shows the page", lambda: client.tab().get("title") == "/page")
        client.watch_frame_targets()

        # The preset is in force: the page says Safari, and so did the request for it.
        preset_user_agent = client.evaluate("navigator.userAgent")
        assert "Safari/" in preset_user_agent and "Ladybird/" not in preset_user_agent, preset_user_agent
        assert "Ladybird/" not in requests_seen["/page"], requests_seen["/page"]

        # A force_Ladybird rule for this site takes over both, with no reload: the page's own string and the wire's.
        rule = json.dumps([{"matches": [f"{base}/*"], "user_agent": ["force_Ladybird"]}])
        client.evaluate(f"internals.setSiteCompatibilityData({json.dumps(rule)})")
        forced_user_agent = client.evaluate("navigator.userAgent")
        assert "Ladybird/" in forced_user_agent, forced_user_agent
        client.evaluate("fetch('/forced').then(() => { window.forcedDone = true; })")
        wait_until("the forced fetch arrives", lambda: "/forced" in requests_seen)
        assert requests_seen["/forced"] == forced_user_agent, (requests_seen["/forced"], forced_user_agent)

        # Elsewhere, the preset still holds.
        client.evaluate("internals.setSiteCompatibilityData('[]')")
        assert client.evaluate("navigator.userAgent") == preset_user_agent

        print(
            "PASS: A force_Ladybird rule sends Ladybird's own User-Agent despite the preset, on the page and on the wire"
        )
    finally:
        client.close()
        stop(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            test_force_ladybird_overrides_the_preset(ladybird_binary, Path(directory) / "profile", server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
