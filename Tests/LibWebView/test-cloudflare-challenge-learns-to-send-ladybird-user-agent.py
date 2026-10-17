#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
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

# The User-Agent of every request each path received, in order.
requests_seen: dict[str, list[str]] = {}
requests_seen_lock = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    # A site behind Cloudflare, as a browser that spoofs its User-Agent sees it: a request naming a browser Cloudflare
    # knows gets the challenge page, since the rest of the request isn't that browser's; one naming Ladybird is served.
    def do_GET(self):
        user_agent = self.headers.get("User-Agent", "")
        with requests_seen_lock:
            requests_seen.setdefault(self.path, []).append(user_agent)
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if "Ladybird/" not in user_agent:
            self.send_response(403)
            self.send_header("Content-Type", "text/html")
            self.send_header("cf-mitigated", "challenge")
            self.send_header("server", "cloudflare")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(b"<!doctype html><title>Just a moment...</title>")
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(f"<!doctype html><title>{self.path}</title>".encode())

    def log_message(self, format, *args):
        pass


def launch(ladybird_binary, profile, url):
    # Like the DevTools tests' launch(), with the User-Agent preset the test needs.
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


def user_agents_for(path):
    with requests_seen_lock:
        return list(requests_seen.get(path, []))


def test_a_challenged_site_is_learned(ladybird_binary, profile, server):
    base = f"http://127.0.0.1:{server.server_port}"
    process, client = launch(ladybird_binary, profile, f"{base}/page")
    try:
        # The page loaded: the request claiming Safari was challenged, and the retry as Ladybird was served.
        wait_until("the tab shows the page", lambda: client.tab().get("title") == "/page")
        client.watch_frame_targets()
        [challenged, served] = user_agents_for("/page")
        assert "Safari/" in challenged and "Ladybird/" not in challenged, challenged
        assert "Ladybird/" in served, served

        # The document says what its request said.
        assert "Ladybird/" in client.evaluate("navigator.userAgent")

        # The site is learned: the next navigation to it says Ladybird from the start.
        client.evaluate(f"location.assign('{base}/second')")
        wait_until("the second page shows", lambda: client.tab().get("title") == "/second")
        assert user_agents_for("/second") == [user_agents_for("/page")[1]], user_agents_for("/second")
    finally:
        client.close()
        stop(process)

    # And remembered by the profile: a fresh launch says Ladybird to the site from its first request.
    process, client = launch(ladybird_binary, profile, f"{base}/third")
    try:
        wait_until("the third page shows", lambda: client.tab().get("title") == "/third")
        third = user_agents_for("/third")
        assert len(third) == 1 and "Ladybird/" in third[0], third

        print("PASS: A site that challenges a spoofed User-Agent is retried as Ladybird, learned, and remembered")
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
            test_a_challenged_site_is_learned(ladybird_binary, Path(directory) / "profile", server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
