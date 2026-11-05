#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import runpy
import socket
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

FRAME_COUNT = 8
RULE_COUNT = 20000

# Bytes per rule of style data each further frame may add beyond an empty frame, by how it links the sheet, which a
# change growing them has to raise. Style data is what the style engines report, each allocation once however many
# frames share it, which unlike the RSS does not depend on what nothing has collected yet. An "own-first" frame links a
# small sheet of its own, of a different length in each frame, before the shared one, so the frames number the shared
# sheet's rules apart.
PER_FRAME_BYTES_PER_RULE = {"link": 8, "print": 8, "own-first": 80}

# A sheet with a url() value, which resolves against the sheet's own URL. Every frame links the same sheet, from a
# document at a URL of its own.
#
# NB: All the rules name one image. Each distinct image takes about 40 KB of a frame's image requests, which is no more
#     a cost per rule than it is style data.
STYLE_SHEET = "\n".join(
    f".c{i} .d{i % 97} > span.e{i % 13}:hover{{color:#{i % 4096:03x};background:url(img.png)}}"
    for i in range(RULE_COUNT)
).encode()


class TestPageServer(http.server.ThreadingHTTPServer):
    request_queue_size = socket.SOMAXCONN


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.endswith("/sheet.css"):
            body = STYLE_SHEET
            content_type = "text/css"
        elif self.path.endswith("/own.css"):
            frame = int(self.path.split("/")[3])
            body = "\n".join(f".own{frame}-{i} {{ margin-top: {i}px }}" for i in range(10 + frame)).encode()
            content_type = "text/css"
        elif self.path.startswith("/frame/"):
            kind = self.path.split("/")[2]
            link = {
                "link": '<link rel=stylesheet href="/sheet.css">',
                "print": '<link rel=stylesheet media=print href="/sheet.css">',
                "own-first": '<link rel=stylesheet href="own.css"><link rel=stylesheet href="/sheet.css">',
                "empty": "",
            }[kind]
            body = f"<!doctype html>{link}<div class=c1><div class=d1><span class=e1>x</span></div></div>".encode()
            content_type = "text/html"
        elif self.path.startswith("/?frames="):
            count, kind = self.path.split("=", 1)[1].split("&kind=")
            count = int(count)
            frames = "".join(f'<iframe src="/frame/{kind}/{i}/"></iframe>' for i in range(count))
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


def measure(webdriver_binary, server_port, frame_count, kind="link"):
    with tempfile.TemporaryDirectory(prefix="ladybird-shared-sheet-") as profile:
        process, port = helpers["launch_webdriver"](
            webdriver_binary, "--headless", "--expose-internals-object", f"--profile-path={profile}"
        )
        session = None
        try:
            session = helpers["create_session"](port)
            status, _, raw = helpers["request"](
                port,
                "POST",
                f"/session/{session}/url",
                {"url": f"http://127.0.0.1:{server_port}/?frames={frame_count}&kind={kind}"},
            )
            assert status == 200, raw
            # Every frame has applied the sheet once its document has laid out with it.
            script = """
                return Array.from(document.querySelectorAll("iframe")).every(frame => {
                    const span = frame.contentDocument && frame.contentDocument.querySelector("span");
                    if (!span || frame.contentDocument.readyState !== "complete")
                        return false;
                    span.offsetWidth;
                    const sheets = frame.contentDocument.styleSheets;
                    if (arguments[1] === "empty")
                        return sheets.length === 0;
                    const shared = arguments[1] === "own-first" ? 1 : 0;
                    return sheets.length === shared + 1 && sheets[shared].cssRules.length === arguments[0];
                });
            """
            deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
            payload = {"value": None}
            while time.monotonic() < deadline:
                status, payload, raw = helpers["request"](
                    port,
                    "POST",
                    f"/session/{session}/execute/sync",
                    {"script": script, "args": [RULE_COUNT, kind]},
                )
                assert status == 200, raw
                if payload["value"] is True:
                    break
                time.sleep(0.2)
            assert payload["value"] is True, f"The {frame_count} frames never finished loading the sheet"
            status, payload, raw = helpers["request"](
                port,
                "POST",
                f"/session/{session}/execute/sync",
                {"script": "return internals.styleMemoryOfFrames();", "args": []},
            )
            assert status == 200, raw
            style_bytes = payload["value"]
            time.sleep(2)
            return web_content_resident_bytes(process.pid), style_bytes
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

    server = TestPageServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        measured = {
            kind: (
                measure(webdriver_binary, server.server_port, 1, kind),
                measure(webdriver_binary, server.server_port, FRAME_COUNT, kind),
            )
            for kind in ("empty", "link", "print", "own-first")
        }
    finally:
        server.shutdown()
        server.server_close()

    (one, _), (many, _) = measured["link"]
    # The frames share one parse of the sheet, so the page costs little more than one frame does. Parsing the sheet
    # once per frame would cost several times that.
    assert many < one * 2, f"{FRAME_COUNT} frames of one sheet take {many >> 20} MiB, one frame {one >> 20} MiB"

    def per_extra_frame(kind):
        (_, one), (_, many) = measured[kind]
        return (many - one) / (FRAME_COUNT - 1)

    # What each further frame linking the sheet holds of style data beyond an empty frame, per rule: the document's own
    # index of the sheet and whatever else it holds per rule.
    empty = per_extra_frame("empty")
    failures = []
    for kind, budget in PER_FRAME_BYTES_PER_RULE.items():
        bytes_per_rule = (per_extra_frame(kind) - empty) / RULE_COUNT
        print(f"{kind}: {bytes_per_rule:.1f} B/rule per extra frame (budget {budget})")
        if bytes_per_rule > budget:
            failures.append(kind)
    assert not failures, f"over their per-frame budgets: {failures}"
    print(f"PASS: {FRAME_COUNT} frames sharing a sheet: {many >> 20} MiB, one frame: {one >> 20} MiB")


if __name__ == "__main__":
    main()
