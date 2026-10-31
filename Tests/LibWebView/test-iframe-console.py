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
PAGE = b"""<!doctype html><title>iframe-console</title>
<iframe srcdoc="<script>console.log('logged by the iframe at load', { early: 1 }); window.loggedAtLoad = true;</script>"></iframe>
<iframe id="blank"></iframe>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
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
    url = f"http://127.0.0.1:{server.server_port}/page"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-iframe-console-") as profile:
            process, _, client = devtools["launch"](ladybird_binary, profile, url)
            try:
                devtools["wait_for_page"](client, url, "iframe-console")
                watcher = client.watch_frame_targets()
                messages = []
                receive = client.receive

                def receive_and_record_messages():
                    packet = receive()
                    if packet.get("type") == "resources-available-array":
                        for resource_type, resources in packet.get("array", []):
                            if resource_type == "console-message":
                                messages.extend(resources)
                    return packet

                def message_logged(text):
                    return next((message for message in messages if message["arguments"][0] == text), None)

                # Keep console resources even if they arrive while evaluation is retried after a target change.
                client.receive = receive_and_record_messages

                # What the iframe logged before the client watched arrives once it does.
                devtools["wait_until"](
                    "the page has loaded, and the iframe has logged",
                    lambda: client.evaluate("document.readyState === 'complete' && frames[0].loggedAtLoad === true"),
                )
                client.send({"to": watcher, "type": "watchResources", "resourceTypes": ["console-message"]})
                client.receive_matching(lambda packet: message_logged("logged by the iframe at load") is not None)

                # What the iframe logs afterward arrives as it comes, with objects the client can expand. The server
                # replaces the frame target when a load finishes, and the old target's object actors go with it; the
                # root actor answers a request to one of them with unknownActor, so log a fresh object then.
                def expand_logged_object():
                    for attempt in range(5):
                        text = f"logged by the iframe later ({attempt})"
                        client.evaluate(f"frames[0].console.log('{text}', {{ late: 2 }}); undefined")
                        if message_logged(text) is None:
                            client.receive_matching(lambda packet, text=text: message_logged(text) is not None)
                        logged = message_logged(text)
                        assert logged is not None, messages
                        grip = logged["arguments"][1]
                        assert grip.get("class") == "Object" and "actor" in grip, grip

                        actor = grip["actor"]
                        client.send({"to": actor, "type": "prototypeAndProperties"})
                        reply = client.receive_matching(
                            lambda packet, actor=actor: (
                                packet.get("from") == actor
                                or (packet.get("error") == "unknownActor" and f"'{actor}'" in packet.get("message", ""))
                            )
                        )
                        if reply.get("from") == actor:
                            return reply
                    raise AssertionError("The logged object's actor went away five times")

                properties = expand_logged_object()
                assert "late" in properties.get("ownProperties", {}), properties

                # An iframe that stays on its first document, about:blank, logs to the tab's console too.
                client.evaluate(
                    "document.getElementById('blank').contentWindow.console.log('logged by about:blank'); 0"
                )
                if message_logged("logged by about:blank") is None:
                    client.receive_matching(lambda packet: message_logged("logged by about:blank") is not None)
                print("PASS: An iframe's console messages reach the tab's console, and its logged objects expand")
            finally:
                client.close()
                devtools["stop"](process)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
