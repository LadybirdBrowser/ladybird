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
PAGE = b"""<!doctype html><title>worker-console</title>
<script>
window.onerror = (message, filename, line, column) => {
    window.lastError = { message, filename, line, column };
};
</script>
"""
WORKER = b'\nthrow new Error("worker console sentinel");\n'


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript" if self.path == "/worker.js" else "text/html")
        self.end_headers()
        self.wfile.write(WORKER if self.path == "/worker.js" else PAGE)

    def log_message(self, format, *args):
        pass


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    url = origin + "/page"
    worker_url = origin + "/worker.js"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-worker-console-") as profile:
            process, _, client = devtools["launch"](ladybird_binary, profile, url)
            try:
                devtools["wait_for_page"](client, url, "worker-console")
                client.watch_frame_targets()
                errors = []
                receive = client.receive

                def receive_and_record_errors():
                    packet = receive()
                    if packet.get("type") == "resources-available-array":
                        for resource_type, resources in packet.get("array", []):
                            if resource_type == "error-message":
                                errors.extend(resource["pageError"] for resource in resources)
                    return packet

                # Keep error resources even if they arrive while evaluation is retried after a target change.
                client.receive = receive_and_record_errors
                client.evaluate("window.worker = new Worker('/worker.js'); undefined")
                if not errors:
                    client.receive_matching(lambda packet: bool(errors))
                preview = errors[0]["exception"]["preview"]
                event = json.loads(client.evaluate("JSON.stringify(window.lastError)"))
                assert "worker console sentinel" in event["message"], event
                assert preview["message"] == event["message"], preview
                assert event["filename"] == worker_url, event
                assert event["line"] == 2 and event["column"] > 0, event
                expected_location = f"@{worker_url}:{event['line']}:{event['column']}\n"
                assert preview.get("stack") == expected_location, preview
                assert not errors[0]["isPromiseRejection"], errors[0]
                print("PASS: An unhandled worker exception keeps its message and source location in the console")
            finally:
                client.close()
                devtools["stop"](process)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
