#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import runpy
import subprocess
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PDF = (
    b"%PDF-1.1\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n"
    b"3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>endobj\ntrailer<</Root 1 0 R>>\n%%EOF\n"
)


class PdfHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/pdf")
        self.send_header("Content-Length", str(len(PDF)))
        self.end_headers()
        self.wfile.write(PDF)

    def log_message(self, format, *args):
        pass


# The PDF viewer runs with an opaque origin of its own, so the viewers of PDFs from different sites share no storage:
# The viewer is refused localStorage.
def run_test(webdriver_binary):
    server = helpers["LocalHTTPServer"](("127.0.0.1", 0), PdfHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    with tempfile.TemporaryDirectory() as directory:
        process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(path, body):
                status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def evaluate(script):
                return command("execute/sync", {"script": script, "args": []})

            url = f"http://127.0.0.1:{server.server_address[1]}/document.pdf"
            command("url", {"url": url})

            # The viewer took the place of the initial about:blank, whose origin is opaque too.
            location = evaluate("return location.href;")
            assert location == url, f"The navigation ended at {location}"
            origin = evaluate("return self.origin;")
            assert origin == "null", f"The viewer has the origin {origin}"
            # NB: pdf.js gets a localStorage of its own, which lasts as long as the viewer.
            storage = evaluate("try { sessionStorage.length; return 'no exception'; } catch (e) { return e.name; }")
            assert storage == "SecurityError", f"The viewer's sessionStorage gave {storage}"
            print("PASS: the PDF viewer has an opaque origin and no storage")
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
