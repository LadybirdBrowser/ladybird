#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import tempfile
import threading

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
launch = devtools["launch"]
stop = devtools["stop"]
wait_for_page = devtools["wait_for_page"]


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(f"<!doctype html><title>{self.path}</title>".encode())

    def log_message(self, format, *args):
        pass


def window_geometry(client):
    return json.loads(client.evaluate("JSON.stringify([outerWidth, outerHeight, innerWidth, innerHeight])"))


def test_window_size_reaches_every_page(ladybird_binary, profile, server):
    source_url = f"http://127.0.0.1:{server.server_port}/source"
    target_url = f"http://localhost:{server.server_port}/target"
    process, _devtools_port, client = launch(ladybird_binary, profile, source_url)
    try:
        wait_for_page(client, source_url, "/source")
        client.watch_frame_targets()

        # A headless window is exactly its viewport, so the page's window size is its viewport size.
        outer_width, outer_height, inner_width, inner_height = window_geometry(client)
        assert outer_width > 0 and outer_height > 0, (outer_width, outer_height)
        assert (outer_width, outer_height) == (inner_width, inner_height), (
            outer_width,
            outer_height,
            inner_width,
            inner_height,
        )

        # A cross-site navigation moves the tab into a page of another process, which is told the window size the
        # tab's view last got, like every page the tab comes to be hosted by.
        client.evaluate(f"setTimeout(() => location.href = {json.dumps(target_url)}, 0)")
        wait_for_page(client, target_url, "/target")
        geometry = window_geometry(client)
        assert geometry == [outer_width, outer_height, inner_width, inner_height], geometry

        print("PASS: Every page the tab is hosted by reports the window size")
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
            test_window_size_reaches_every_page(ladybird_binary, Path(directory) / "profile", server)
    finally:
        server.shutdown()


if __name__ == "__main__":
    main()
