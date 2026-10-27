#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import subprocess
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_test(webdriver_binary):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<!DOCTYPE html><title>set permission</title>")

        def log_message(self, format, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(json.dumps({"firstRunComplete": True}))
        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
            session = helpers["create_session"](port)

            def request(path, body):
                return helpers["request"](port, "POST", f"/session/{session}/{path}", body)

            def query(name):
                status, payload, raw = request(
                    "execute/async",
                    {
                        "script": """
                        const done = arguments[1];
                        navigator.permissions.query({ name: arguments[0] }).then(status => done(status.state));
                    """,
                        "args": [name],
                    },
                )
                assert status == 200, raw
                return payload["value"]

            status, _, raw = request("url", {"url": f"http://127.0.0.1:{server.server_address[1]}/"})
            assert status == 200, raw

            for state in ("denied", "granted", "prompt"):
                status, payload, raw = request("permissions", {"descriptor": {"name": "geolocation"}, "state": state})
                assert status == 200, raw
                assert payload["value"] is None, raw
                assert query("geolocation") == state, (state, query("geolocation"))

            for body in (
                {"descriptor": {"name": "camera"}, "state": "granted"},
                {"descriptor": {"name": "geolocation"}, "state": "maybe"},
                {"descriptor": "geolocation", "state": "granted"},
                {"state": "granted"},
            ):
                status, payload, raw = request("permissions", body)
                assert status == 400, raw
                assert payload["value"]["error"] == "invalid argument", raw

            print("PASS: Set Permission sets the current site's permission, and rejects invalid arguments")
        finally:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            server.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
