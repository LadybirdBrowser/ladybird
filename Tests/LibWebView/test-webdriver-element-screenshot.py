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
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PAGE = b"""<!doctype html>
<style>
html, body { margin: 0; background: white; }
#target { position: absolute; left: 300px; top: 200px; width: 100px; height: 50px; background: red; }
#offscreen { position: absolute; left: 0; top: -20px; width: 100px; height: 50px; background: red; }
#fixed { position: fixed; left: 500px; top: 0; width: 100px; height: 50px; background: red; }
#spacer { height: 3000px; }
</style>
<div id="target"></div>
<div id="offscreen"></div>
<div id="fixed"></div>
<div id="spacer"></div>
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


# The page decodes the screenshot itself, and reports its size and the colors of the pixels the test looks at.
DECODE_SCRIPT = """
const [data, points, done] = arguments;
const image = new Image();
image.onload = () => {
    const canvas = document.createElement("canvas");
    canvas.width = image.width;
    canvas.height = image.height;
    const context = canvas.getContext("2d");
    context.drawImage(image, 0, 0);
    const colors = points.map(([x, y]) => Array.from(context.getImageData(x, y, 1, 1).data));
    done({ width: image.width, height: image.height, colors });
};
image.onerror = () => done(null);
image.src = "data:image/png;base64," + data;
"""


def main():
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as directory:
        # The pixels are colors the display list carries, which take no GPU to draw.
        process, port = helpers["launch_webdriver"](
            sys.argv[1], "--headless", "--force-cpu-painting", "--profile-path", directory
        )
        session = None
        try:
            session = helpers["create_session"](port)

            def command(method, path, body=None):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            command("POST", "window/rect", {"width": 800, "height": 600})
            url = f"http://127.0.0.1:{server.server_port}/"
            command("POST", "url", {"url": url})
            deadline = time.monotonic() + 30
            while command("POST", "execute/sync", {"script": "return document.readyState", "args": []}) != "complete":
                assert time.monotonic() < deadline, "The page did not finish loading"
                time.sleep(0.01)

            element = command("POST", "element", {"using": "css selector", "value": "#target"})
            element_id = next(iter(element.values()))
            screenshot = command("GET", f"element/{element_id}/screenshot")

            # https://w3c.github.io/webdriver/#dfn-draw-a-bounding-box-from-the-framebuffer
            # The image starts at the element's position, so the element is at its top-left corner.
            result = command(
                "POST",
                "execute/async",
                {"script": DECODE_SCRIPT, "args": [screenshot, [[0, 0], [99, 49], [100, 50]]]},
            )
            assert result is not None, "The element screenshot isn't a decodable PNG"
            red, white = [255, 0, 0, 255], [255, 255, 255, 255]
            assert result["colors"][0] == red, f"Top-left pixel isn't the element: {result}"
            assert result["colors"][1] == red, f"The element's bottom-right pixel isn't red: {result}"
            assert result["colors"][2] == white, f"The pixel past the element isn't the page: {result}"
            print("PASS: the element screenshot starts at the element")

            # An element that starts above the viewport has nothing drawn for the rows above it.
            element = command("POST", "element", {"using": "css selector", "value": "#offscreen"})
            screenshot = command("GET", f"element/{next(iter(element.values()))}/screenshot")
            result = command(
                "POST", "execute/async", {"script": DECODE_SCRIPT, "args": [screenshot, [[0, 0], [0, 20]]]}
            )
            assert result is not None, "The offscreen element's screenshot isn't a decodable PNG"
            assert result["colors"][0][3] == 0, f"The rows above the viewport aren't transparent: {result}"
            assert result["colors"][1] == red, f"The element's first visible row isn't red: {result}"
            print("PASS: the part of an element outside the viewport is transparent")

            # On a scrolled page, an element is found where the viewport shows it.
            command("POST", "execute/sync", {"script": "window.scrollTo(0, 100)", "args": []})
            element = command("POST", "element", {"using": "css selector", "value": "#fixed"})
            screenshot = command("GET", f"element/{next(iter(element.values()))}/screenshot")
            result = command(
                "POST", "execute/async", {"script": DECODE_SCRIPT, "args": [screenshot, [[0, 0], [99, 49]]]}
            )
            assert result is not None, "The fixed element's screenshot isn't a decodable PNG"
            assert result["colors"] == [red, red], (
                f"The fixed element isn't at the top-left on a scrolled page: {result}"
            )
            print("PASS: the element screenshot of a scrolled page starts at the element")
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
