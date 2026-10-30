#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import base64
import http.server
import json
import runpy
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(
            b"<!doctype html><title>Force-dark test</title><body style='background: white'>"
            b"<script>window.initialDark = matchMedia('(prefers-color-scheme: dark)').matches;</script>"
            b"<iframe srcdoc=\"<body style='background:white'>\" style='position:absolute;left:100px;top:100px;border:0'></iframe>"
        )

    def log_message(self, format, *args):
        pass


def png_pixel(png, x, y):
    assert png[:8] == b"\x89PNG\r\n\x1a\n" and png[12:16] == b"IHDR", "Screenshot is not a PNG"
    width, _, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", png[16:29])
    assert bit_depth == 8 and color_type in (2, 6) and interlace == 0, "Unsupported PNG format"
    channels = 4 if color_type == 6 else 3
    offset = 8
    data = b""
    while offset < len(png):
        (length,) = struct.unpack(">I", png[offset : offset + 4])
        if png[offset + 4 : offset + 8] == b"IDAT":
            data += png[offset + 8 : offset + 8 + length]
        offset += 12 + length
    raw = zlib.decompress(data)
    stride = width * channels
    previous = bytearray(stride)
    for row in range(y + 1):
        filter_type = raw[row * (stride + 1)]
        line = bytearray(raw[row * (stride + 1) + 1 : (row + 1) * (stride + 1)])
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + (left + up) // 2) & 0xFF
            elif filter_type == 4:
                estimate = left + up - up_left
                distances = (abs(estimate - left), abs(estimate - up), abs(estimate - up_left))
                predictor = (left, up, up_left)[distances.index(min(distances))]
                line[i] = (line[i] + predictor) & 0xFF
        previous = line
    return tuple(previous[x * channels : x * channels + 3])


def run_test(default):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(
            json.dumps(
                {
                    "appearance": {"websiteColorScheme": "light"},
                    "content": {"enableForceDark": default, "forceDarkPerHost": {"localhost": not default}},
                }
            )
        )
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

            def command(method, path, body=None):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                return status, payload, raw

            def wait_for_page(url):
                deadline = time.monotonic() + 30
                state = None
                while time.monotonic() < deadline:
                    status, payload, raw = command(
                        "POST", "execute/sync", {"script": "return [location.href, document.readyState]", "args": []}
                    )
                    if (
                        status == 500
                        and payload["value"]["message"] == "WebContent was replaced while executing the command"
                    ):
                        continue
                    assert status == 200, raw
                    state = payload["value"]
                    if state == [url, "complete"]:
                        return
                    time.sleep(0.01)
                raise AssertionError(f"Expected {url}, got {state}")

            def assert_force_dark(description, expected, check_initial=True):
                status, payload, raw = command("GET", "screenshot")
                assert status == 200, raw
                color = png_pixel(base64.b64decode(payload["value"]), 10, 10)
                assert (max(color) < 128) == expected, f"{description} expected dark={expected}, got {color}"
                iframe_color = png_pixel(base64.b64decode(payload["value"]), 110, 110)
                assert (max(iframe_color) < 128) == expected, f"{description} iframe painted {iframe_color}"
                status, payload, raw = command(
                    "POST",
                    "execute/sync",
                    {
                        "script": "return window.initialDark"
                        if check_initial
                        else "return matchMedia('(prefers-color-scheme: dark)').matches",
                        "args": [],
                    },
                )
                assert status == 200, raw
                assert payload["value"] == expected, f"{description} initial color preference was {payload['value']}"

            source_url = f"http://127.0.0.1:{server.server_port}/source"
            target_url = f"http://localhost:{server.server_port}/target"

            status, _, raw = command("POST", "url", {"url": source_url})
            assert status == 200, raw
            wait_for_page(source_url)
            assert_force_dark("The tab's first page", default)

            # A cross-site navigation moves the tab into a page of another process.
            status, _, raw = command("POST", "url", {"url": target_url})
            assert status == 200, raw
            wait_for_page(target_url)
            assert_force_dark("A cross-site navigation's page", not default)

            status, _, raw = command("POST", "back", {})
            assert status == 200, raw
            wait_for_page(source_url)
            assert_force_dark("A cross-site traversal's page", default)
            status, _, raw = command("POST", "url", {"url": target_url + "-same-host"})
            assert status == 200, raw
            wait_for_page(target_url + "-same-host")
            status, _, raw = command("POST", "url", {"url": target_url + "-same-host-next"})
            assert status == 200, raw
            wait_for_page(target_url + "-same-host-next")
            assert_force_dark("Same-host navigation", not default)
            status, _, raw = command("POST", "refresh", {})
            assert status == 200, raw
            wait_for_page(target_url + "-same-host-next")
            assert_force_dark("Reload", not default)
            print(f"PASS: per-host force-dark across navigation (default={default})")

            def evaluate(script):
                status, payload, raw = command("POST", "execute/sync", {"script": script, "args": []})
                assert status == 200, raw
                return payload["value"]

            def wait_for_condition(script):
                deadline = time.monotonic() + 10
                while time.monotonic() < deadline:
                    if evaluate(script):
                        return
                    time.sleep(0.01)
                raise AssertionError(f"Condition did not become true: {script}")

            status, payload, raw = command("GET", "window")
            assert status == 200, raw
            site_handle = payload["value"]
            status, payload, raw = command("POST", "window/new", {"type": "tab"})
            assert status == 200, raw
            settings_handle = payload["value"]["handle"]

            def switch_to(handle):
                status, _, raw = command("POST", "window", {"handle": handle})
                assert status == 200, raw

            switch_to(settings_handle)
            status, _, raw = command("POST", "url", {"url": "about:settings"})
            assert status == 200, raw
            wait_for_page("about:settings")
            wait_for_condition("return document.querySelector('#force-dark-sites button') !== null")
            assert evaluate(
                "return document.querySelector('#force-dark-sites').textContent.includes('localhost: ' + "
                + ("'Off'" if default else "'On'")
                + ")"
            )
            evaluate("document.querySelector('#enable-force-dark').click()")
            wait_for_condition(
                "return document.querySelector('#enable-force-dark').checked === " + str(not default).lower()
            )
            switch_to(site_handle)
            assert_force_dark("Explicit choice survives a global toggle", not default, check_initial=False)
            switch_to(settings_handle)
            evaluate("document.querySelector('#enable-force-dark').click()")
            wait_for_condition(
                "return document.querySelector('#enable-force-dark').checked === " + str(default).lower()
            )
            evaluate("document.querySelector('#force-dark-sites button').click()")
            wait_for_condition("return document.querySelector('#force-dark-sites button') === null")
            switch_to(site_handle)
            wait_for_condition("return matchMedia('(prefers-color-scheme: dark)').matches === " + str(default).lower())
            assert_force_dark("Reset updates an already-open page", default, check_initial=False)
            saved = json.loads((config / "Settings.json").read_text())["content"]
            assert "localhost" not in saved["forceDarkPerHost"]
            print(f"PASS: settings toggle and per-host reset (default={default})")
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


for default in (True, False):
    run_test(default)
