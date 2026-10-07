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

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

FRAME_COUNT = 8
RULE_COUNT = 20000

# A sheet with url() values, which resolve against the sheet's own URL. Every frame links the same sheet, from a
# document at a URL of its own.
STYLE_SHEET = "\n".join(
    f".c{i} .d{i % 97} > span.e{i % 13}:hover{{color:#{i % 4096:03x};background:url(img{i % 50}.png)}}"
    for i in range(RULE_COUNT)
).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith("/sheet.css"):
            body = STYLE_SHEET
            content_type = "text/css"
        elif self.path.startswith("/frame/"):
            body = b'<!doctype html><link rel=stylesheet href="/sheet.css"><div class=c1><div class=d1><span class=e1>x</span></div></div>'
            content_type = "text/html"
        elif self.path.startswith("/?frames="):
            count = int(self.path.rsplit("=", 1)[1])
            frames = "".join(f'<iframe src="/frame/{i}/"></iframe>' for i in range(count))
            body = f"<!doctype html><title>frames={count}</title>{frames}".encode()
            content_type = "text/html"
        else:
            # The sheet's images, among others; the test never needs them to load.
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def resident_bytes(pid):
    if sys.platform == "darwin":
        output = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], capture_output=True, text=True).stdout
        return int(output.strip() or 0) * 1024
    for line in open(f"/proc/{pid}/status"):
        if line.startswith("RssAnon:"):
            return int(line.split()[1]) * 1024
    return 0


def web_content_resident_bytes(root_pid):
    output = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True).stdout
    children = {}
    for pid, ppid, command in (line.split(None, 2) for line in output.splitlines()):
        children.setdefault(int(ppid), []).append((int(pid), command))
    largest = 0
    pending = [root_pid]
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            if command.split()[0].endswith("/WebContent"):
                largest = max(largest, resident_bytes(pid))
    return largest


def measure(webdriver_binary, server_port, frame_count):
    with tempfile.TemporaryDirectory(prefix="ladybird-shared-sheet-") as profile:
        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", f"--profile-path={profile}", "-l", "127.0.0.1", "-p", str(port)]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
            session = helpers["create_session"](port)
            status, _, raw = helpers["request"](
                port,
                "POST",
                f"/session/{session}/url",
                {"url": f"http://127.0.0.1:{server_port}/?frames={frame_count}"},
            )
            assert status == 200, raw
            # Every frame has applied the sheet once its document has laid out with it.
            script = """
                return Array.from(document.querySelectorAll("iframe")).every(frame => {
                    const span = frame.contentDocument && frame.contentDocument.querySelector("span");
                    return span && frame.contentDocument.readyState === "complete"
                        && frame.contentDocument.styleSheets.length === 1
                        && frame.contentDocument.styleSheets[0].cssRules.length === arguments[0];
                });
            """
            deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
            payload = {"value": None}
            while time.monotonic() < deadline:
                status, payload, raw = helpers["request"](
                    port, "POST", f"/session/{session}/execute/sync", {"script": script, "args": [RULE_COUNT]}
                )
                assert status == 200, raw
                if payload["value"] is True:
                    break
                time.sleep(0.2)
            assert payload["value"] is True, f"The {frame_count} frames never finished loading the sheet"
            time.sleep(2)
            return web_content_resident_bytes(process.pid)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    webdriver_binary = parser.parse_args().webdriver_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        one = measure(webdriver_binary, server.server_port, 1)
        many = measure(webdriver_binary, server.server_port, FRAME_COUNT)
    finally:
        server.shutdown()
        server.server_close()

    # The frames share one parse of the sheet, so the page costs little more than one frame does. Parsing the sheet
    # once per frame would cost several times that.
    assert many < one * 2, f"{FRAME_COUNT} frames of one sheet take {many >> 20} MiB, one frame {one >> 20} MiB"
    print(f"PASS: {FRAME_COUNT} frames sharing a sheet: {many >> 20} MiB, one frame: {one >> 20} MiB")


if __name__ == "__main__":
    main()
