#!/usr/bin/env python3

import argparse
import http.server
import json
import os
import socket
import socketserver
import sys
import time

from collections import defaultdict
from typing import Dict
from typing import Optional

"""
Description:
    This script starts a simple HTTP echo server on localhost for use in our in-tree tests.
    The port is assigned by the OS on startup and printed to stdout.

Endpoints:
    - POST /echo <json body>, Creates an echo response for later use. See "Echo" class below for body properties.
"""


class Echo:
    method: str
    path: str
    status: int
    headers: Dict[str, str]
    body: Optional[str]
    delay_ms: Optional[int]
    reason_phrase: Optional[str]
    reflect_headers_in_body: bool


# In-memory store for echo responses
echo_store: Dict[str, Echo] = {}


class TestHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *arguments, **kwargs):
        super().__init__(*arguments, directory=None, **kwargs)

    def do_GET(self):
        if self.path.startswith("/static/"):
            # Remove "/static/" prefix and use built-in method
            self.path = self.path[7:]
            return super().do_GET()
        else:
            self.handle_echo()

    def do_POST(self):
        if self.path == "/echo":
            content_length = int(self.headers["Content-Length"])
            post_data = self.rfile.read(content_length)
            data = json.loads(post_data.decode("utf-8"))

            echo = Echo()
            echo.method = data.get("method", None)
            echo.path = data.get("path", None)
            echo.status = data.get("status", None)
            echo.body = data.get("body", None)
            echo.delay_ms = data.get("delay_ms", None)
            echo.headers = data.get("headers", {})
            echo.reason_phrase = data.get("reason_phrase", None)
            echo.reflect_headers_in_body = data.get("reflect_headers_in_body", False)

            is_using_reserved_path = echo.path.startswith("/static") or echo.path.startswith("/echo")

            # Return 400: Bad Request if invalid params are given or a reserved path is given
            if (
                echo.method is None
                or echo.path is None
                or echo.status is None
                or (echo.body is not None and "$HEADERS" not in echo.body and echo.reflect_headers_in_body)
                or is_using_reserved_path
            ):
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                return

            # Return 409: Conflict if the method+path combination already exists
            key = f"{echo.method} {echo.path}"
            if key in echo_store:
                self.send_response(409)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                return

            echo_store[key] = echo

            host = self.headers.get("host", "localhost")
            path = echo.path.lstrip("/")
            fetch_url = f"http://{host}/{path}"

            # The params to use on the client when making a request to the newly created echo endpoint
            fetch_config = {
                "method": echo.method,
                "url": fetch_url,
            }

            self.send_response(201)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(fetch_config).encode("utf-8"))
        elif self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self.handle_echo()

    def do_OPTIONS(self):
        if self.path.startswith("/echo"):
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "*")
            self.send_header("Access-Control-Allow-Headers", "*")
            self.end_headers()
        else:
            self.do_other()

    def do_PUT(self):
        self.do_other()

    def do_HEAD(self):
        self.do_other()

    def do_DELETE(self):
        self.do_other()

    def handle_echo(self):
        method = self.command.upper()
        key = f"{method} {self.path}"

        is_revalidation_request = "If-Modified-Since" in self.headers
        send_not_modified = is_revalidation_request and "X-Ladybird-Respond-With-Not-Modified" in self.headers

        send_incomplete_response = "X-Ladybird-Respond-With-Incomplete-Response" in self.headers

        set_invalid_cookie = "X-Ladybird-Set-Invalid-Cookie" in self.headers

        if key in echo_store:
            echo = echo_store[key]
            response_headers = echo.headers.copy()

            if echo.delay_ms is not None:
                time.sleep(echo.delay_ms / 1000)

            if send_not_modified:
                self.send_response(304)
            else:
                self.send_response_only(echo.status, echo.reason_phrase)

                if is_revalidation_request:
                    # Override the Last-Modified header to prevent cURL from thinking the response is still fresh.
                    response_headers["Last-Modified"] = "Thu, 01 Jan 1970 00:00:00 GMT"
                elif send_incomplete_response:
                    # We emulate an incomplete response by advertising a 10KB file, but only sending 2KB.
                    response_headers["Content-Length"] = str(10 * 1024)

            if set_invalid_cookie:
                response_headers["Set-Cookie"] = "invalid=foo; Domain=\xc3\xa9\x6c\xc3\xa8\x76\x65\xff"

            # Set only the headers defined in the echo definition
            if response_headers:
                for header, value in response_headers.items():
                    self.send_header(header, value)
                self.end_headers()

            if send_not_modified:
                return

            if send_incomplete_response:
                self.wfile.write(b"a" * (2 * 1024))
                self.wfile.flush()

                self.connection.shutdown(socket.SHUT_WR)
                self.connection.close()
                return

            if echo.reflect_headers_in_body:
                headers = defaultdict(list)
                for key in self.headers.keys():
                    headers[key] = self.headers.get_all(key)
                headers = json.dumps(headers)
                response_body = echo.body.replace("$HEADERS", headers) if echo.body else headers
            else:
                response_body = echo.body or ""

            # FIXME: This only supports "Range: bytes=start-end" and "Range: bytes=start-". There are other formats to
            #        support if needed: https://developer.mozilla.org/en-US/docs/Web/HTTP/Reference/Headers/Range#syntax
            if "Range" in self.headers:
                range_value = self.headers["Range"].strip()
                assert range_value.startswith("bytes=")
                assert range_value.count("-") == 1

                range_value = range_value[len("bytes=") :]
                start, end = range_value.split("-")

                if end:
                    response_body = response_body[int(start) : min(int(end), len(response_body))]
                else:
                    response_body = response_body[int(start) :]

            self.wfile.write(response_body.encode("utf-8"))
        else:
            self.send_error(404, f"Echo response not found for {key}")

    def do_other(self):
        if self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self.handle_echo()


def start_server(port, static_directory):
    TestHTTPRequestHandler.static_directory = os.path.abspath(static_directory)
    httpd = socketserver.TCPServer(("127.0.0.1", port), TestHTTPRequestHandler)

    print(httpd.socket.getsockname()[1])
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run a HTTP echo server")
    parser.add_argument(
        "-d",
        "--directory",
        type=str,
        default=".",
        help="Directory to serve static files from",
    )
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=0,
        help="Port to run the server on",
    )
    args = parser.parse_args()

    start_server(port=args.port, static_directory=args.directory)
