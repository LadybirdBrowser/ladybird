#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import runpy
import tempfile
import threading

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))

# The container generates no box, so the unit below it resolves without one: Every full layout leaves that unit's
# style to update.
PAGE = b"""<!doctype html><title>catch-up</title>
<style>
    .container { container-type: inline-size; display: contents; }
    .item { width: 50cqw; }
</style>
<div class="container"><div class="item">item</div></div>
<script>window.loaded = true;</script>
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
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/catch-up"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-devtools-") as profile:
            process, devtools_port, client = devtools["launch"](ladybird_binary, profile, url)
            try:
                try:
                    devtools["wait_for_page"](client, url, "catch-up")
                    # The first client lays the document out once more, to collect the layout inspection data.
                    client.watch_frame_targets()
                    assert client.evaluate("window.loaded") is True
                finally:
                    client.close()
                assert process.poll() is None, "The browser exited"
                print("PASS: A DevTools client's catch-up layout settles")
            finally:
                devtools["stop"](process)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
