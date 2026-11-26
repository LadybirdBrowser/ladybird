#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The browser lets a renderer read a local file, reach cookies the way HTTP does, change the window, or synthesize input
# only while it runs a WebDriver command that needs it. This checks that those commands still work.

import http.server
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PAGE = b"""<!doctype html>
<input id="file" type="file">
<button id="button" onclick="this.textContent = 'clicked'">click</button>
<script>
window.buttonEvents = [];
addEventListener("mousedown", () => buttonEvents.push("mousedown"));
addEventListener("mouseup", () => buttonEvents.push("mouseup"));
</script>
"""

READ_FILE_SCRIPT = """
const done = arguments[arguments.length - 1];
const file = document.getElementById("file").files[0];
if (!file)
    done(null);
else
    file.text().then(text => done({ name: file.name, text }));
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(PAGE)

    def log_message(self, format, *args):
        pass


def main():
    server = helpers["LocalHTTPServer"](("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as directory:
        upload = Path(directory) / "upload.txt"
        upload.write_text("uploaded contents")

        process, port = helpers["launch_webdriver"](sys.argv[1], "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(method, path, body=None):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def find(selector):
                element = command("POST", "element", {"using": "css selector", "value": selector})
                return next(iter(element.values()))

            command("POST", "url", {"url": f"http://127.0.0.1:{server.server_port}/"})
            deadline = time.monotonic() + 30
            while command("POST", "execute/sync", {"script": "return document.readyState", "args": []}) != "complete":
                assert time.monotonic() < deadline, "The page did not finish loading"
                time.sleep(0.01)

            command("POST", f"element/{find('#file')}/value", {"text": str(upload)})
            selected = command("POST", "execute/async", {"script": READ_FILE_SCRIPT, "args": []})
            assert selected == {"name": "upload.txt", "text": "uploaded contents"}, selected
            print("PASS: Element Send Keys selects the file it names")

            command("POST", "cookie", {"cookie": {"name": "session", "value": "secret", "httpOnly": True}})
            command("POST", "cookie", {"cookie": {"name": "other", "value": "value"}})
            names = sorted(cookie["name"] for cookie in command("GET", "cookie"))
            assert names == ["other", "session"], names
            assert command("GET", "cookie/session")["value"] == "secret"
            assert "session" not in command("POST", "execute/sync", {"script": "return document.cookie", "args": []})
            command("DELETE", "cookie/session")
            assert [cookie["name"] for cookie in command("GET", "cookie")] == ["other"]
            command("DELETE", "cookie")
            assert command("GET", "cookie") == []
            print("PASS: the cookie commands reach the cookies of the current document, HttpOnly ones too")

            rect = command("POST", "window/rect", {"width": 700, "height": 500})
            assert (rect["width"], rect["height"]) == (700, 500), rect
            print("PASS: Set Window Rect changes the window")

            # A button pressed where the viewport ends after it shrinks is still released.
            command("POST", "window/rect", {"width": 800, "height": 600})
            pointer_down = {
                "actions": [
                    {
                        "type": "pointer",
                        "id": "mouse",
                        "parameters": {"pointerType": "mouse"},
                        "actions": [
                            {"type": "pointerMove", "x": 700, "y": 500, "origin": "viewport"},
                            {"type": "pointerDown", "button": 0},
                        ],
                    }
                ]
            }
            command("POST", "actions", pointer_down)
            command("POST", "window/rect", {"width": 400, "height": 300})
            command("DELETE", "actions")
            events = command("POST", "execute/sync", {"script": "return buttonEvents", "args": []})
            assert events == ["mousedown", "mouseup"], events
            print("PASS: Release Actions releases a button pressed outside the shrunken viewport")

            command("POST", "window/fullscreen")
            fullscreen = "return document.fullscreenElement === document.documentElement"
            assert command("POST", "execute/sync", {"script": fullscreen, "args": []}), "The document is not fullscreen"
            print("PASS: Fullscreen Window makes the document fullscreen")

            button = find("#button")
            command("POST", f"element/{button}/click")
            assert command("GET", f"element/{button}/text") == "clicked"
            print("PASS: Element Click clicks the element")
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
