#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import http.server
import runpy
import subprocess
import sys
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

# The page changes its DOM from a timer, so tasks that run while a screenshot waits on the frame's process leave its
# layout out of date.
PAGE = b"""<!doctype html>
<div id="text">0</div>
<iframe id="frame"></iframe>
<script>
    frame.src = location.href.replace("127.0.0.1", "localhost") + "frame";
    let count = 0;
    setInterval(() => { text.textContent = String(++count); text.style.paddingTop = (count % 7) + "px"; }, 1);
</script>
"""

FRAME = b"<!doctype html><p>frame</p>"


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(FRAME if self.path.endswith("frame") else PAGE)

    def log_message(self, format, *args):
        pass


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as directory:
        process, port = helpers["launch_webdriver"](sys.argv[1], "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(method, path, body=None):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            # localhost and 127.0.0.1 are different sites, so the frame's document is in a process of its own.
            command("POST", "url", {"url": f"http://127.0.0.1:{server.server_port}/"})
            for _ in range(10):
                assert command("GET", "screenshot"), "The screenshot is empty"
            print("PASS: screenshots of a page with a cross-site frame and a changing document")
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
                server.shutdown()
                server.server_close()


main()
