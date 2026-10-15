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

# The port the test server listens on, for the child frame's cross-site URL.
server_port = 0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.path == "/child":
            # The child frame reports what its own process answers, and what it sends on the wire.
            body = """<!doctype html><title>/child</title><script>
                parent.postMessage(JSON.stringify({ userAgent: navigator.userAgent, languages: navigator.languages }), "*");
            </script>"""
        else:
            body = f"""<!doctype html><title>/parent</title><script>
                window.child = null;
                addEventListener("message", event => {{ window.child = JSON.parse(event.data); }});
            </script><iframe src="http://localhost:{server_port}/child"></iframe>"""
        self.wfile.write(body.encode())

    def log_message(self, format, *args):
        pass


def launch(ladybird_binary, profile, url):
    # Like the DevTools tests' launch(), with the User-Agent preset the test is about.
    deadline = time.monotonic() + TIMEOUT_SECONDS
    for _ in range(5):
        if time.monotonic() >= deadline:
            break
        devtools_port = helpers["unused_port"]()
        process = subprocess.Popen(
            [
                ladybird_binary,
                "--headless=manual",
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


def test_user_agent_preset_reaches_child_frames(ladybird_binary, profile, server):
    parent_url = f"http://127.0.0.1:{server.server_port}/parent"
    process, client = launch(ladybird_binary, profile, parent_url)
    try:
        wait_until("the tab shows the parent", lambda: client.tab().get("title") == "/parent")
        client.watch_frame_targets()

        parent = json.loads(
            client.evaluate("JSON.stringify({ userAgent: navigator.userAgent, languages: navigator.languages })")
        )
        assert "Safari" in parent["userAgent"] and "Ladybird" not in parent["userAgent"], parent

        # The child frame is cross-site, so another process hosts it: it has to answer with the same spoofed
        # User-Agent and the same languages as the page that frames it.
        wait_until("the child frame reports", lambda: client.evaluate("window.child !== null"))
        child = json.loads(client.evaluate("JSON.stringify(window.child)"))
        assert child == parent, (child, parent)

        print("PASS: A child frame in another process gets the tab's User-Agent preset and languages")
    finally:
        client.close()
        stop(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    global server_port
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server_port = server.server_port
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            test_user_agent_preset_reaches_child_frames(ladybird_binary, Path(directory) / "profile", server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
