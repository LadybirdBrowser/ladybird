#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import http.server
import json
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
requested = threading.Event()
release_response = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/blocked":
            requested.set()
            if not release_response.wait(timeout=30):
                self.send_error(500)
                return
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b"<!doctype html><title>Zoom test</title><body>Zoom test")

    def log_message(self, format, *args):
        pass


with tempfile.TemporaryDirectory() as directory:
    config = Path(directory) / "config"
    config.mkdir()
    (config / "Settings.json").write_text(json.dumps({"zoomPerHost": {"127.0.0.1": 1.8}}))
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    port = helpers["unused_port"]()
    process = subprocess.Popen(
        [sys.argv[1], "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
    )
    session = None
    try:
        helpers["wait_for_port"](port)
        session = helpers["create_session"](port)

        def command(path, body):
            status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
            assert status == 200, raw
            return payload["value"]

        def script(source, *args):
            return command("execute/sync", {"script": source, "args": list(args)})

        def wait_for_page(url, pixel_ratio):
            deadline = time.monotonic() + 30
            state = None
            while time.monotonic() < deadline:
                state = script("return [location.href, document.readyState, devicePixelRatio]")
                if state == [url, "complete", pixel_ratio]:
                    return
                time.sleep(0.01)
            raise AssertionError(f"Expected {url} at scale {pixel_ratio}, got {state}")

        # Measure the platform's unzoomed ratio rather than assuming a particular display density.
        baseline_ratio = script("return devicePixelRatio")
        source_url = f"http://127.0.0.1:{server.server_port}/source"
        target_url = f"http://localhost:{server.server_port}/blocked"
        command("url", {"url": source_url})
        wait_for_page(source_url, baseline_ratio * 1.8)

        # Hold the response until the old document has been inspected. No delay determines whether
        # navigation has started or whether it is allowed to commit.
        script(
            "const a = document.createElement('a'); a.href = arguments[0]; document.body.append(a); a.click();",
            target_url,
        )
        assert requested.wait(timeout=30), "Destination request did not arrive"
        state = script("return [location.href, devicePixelRatio]")
        assert state == [source_url, baseline_ratio * 1.8], f"Pending navigation changed the old page's zoom: {state}"
        release_response.set()
        wait_for_page(target_url, baseline_ratio)

        command("back", {})
        wait_for_page(source_url, baseline_ratio * 1.8)

        # A canceled navigation must also leave the source document at its saved zoom.
        requested.clear()
        release_response.clear()
        script("location.href = arguments[0]", target_url)
        assert requested.wait(timeout=30), "Canceled destination request did not arrive"
        script("window.stop()")
        state = script("return [location.href, devicePixelRatio]")
        assert state == [source_url, baseline_ratio * 1.8], f"Canceled navigation changed zoom: {state}"
        release_response.set()
        print("PASS: site zoom follows the committed document")
    finally:
        release_response.set()
        if session is not None:
            helpers["request"](port, "DELETE", f"/session/{session}")
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        server.shutdown()
        server.server_close()
