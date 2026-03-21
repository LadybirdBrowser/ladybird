#!/usr/bin/env python3

import argparse
import base64
import hashlib
import http.server
import json
import os
import socket
import socketserver
import sys
import threading
import time
import urllib.parse

from types import SimpleNamespace
from typing import Any
from typing import Callable
from typing import Dict
from typing import Optional
from typing import cast

"""
Description:
    This script starts a simple HTTP and WebSocket echo server on localhost for use in our in-tree tests.
    The port is assigned by the OS on startup and printed to stdout.

Endpoints:
    - POST /echo <json body>, Creates an echo response for later use. See "Echo" class below for body properties.
    - GET <any path> with an "Upgrade: websocket" header: Performs a WebSocket handshake and then echoes
      every text/binary frame back to the client verbatim.
"""


class Echo:
    method: str
    path: str
    status: int
    headers: Dict[str, str]
    body: Optional[str]
    body_encoding: str
    delay_ms: Optional[int]
    reason_phrase: Optional[str]
    reflect_headers_in_body: bool
    close_connection: bool
    wait_for_unblock: Optional[str]

    def __eq__(self, other):
        if not isinstance(other, Echo):
            return NotImplemented

        return (
            self.method == other.method
            and self.path == other.path
            and self.status == other.status
            and self.body == other.body
            and self.body_encoding == other.body_encoding
            and self.delay_ms == other.delay_ms
            and self.headers == other.headers
            and self.reason_phrase == other.reason_phrase
            and self.reflect_headers_in_body == other.reflect_headers_in_body
            and self.close_connection == other.close_connection
            and self.wait_for_unblock == other.wait_for_unblock
        )


# In-memory store for echo responses
echo_store: Dict[str, Echo] = {}

# Headers from the most recent request at each echo path, queryable via GET /recorded-request-headers<echo-path>.
recorded_request_headers: Dict[str, Dict[str, list]] = {}

# Named events used by tests that need deterministic delayed responses.
unblock_events: Dict[str, threading.Event] = {}

# Per RFC 6455: appended to Sec-WebSocket-Key before hashing to compute the Sec-WebSocket-Accept value.
WEBSOCKET_ACCEPT_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WPTContext:
    def __init__(self, wpt_directory):
        vendored_tools_root = os.path.join(wpt_directory, "_wpttools")

        for path in (wpt_directory, vendored_tools_root):
            sys.path.insert(0, path)

        import _wpttools as vendored_tools
        import localpaths

        # Make the copied tree behave like the upstream `tools` package so
        # vendored modules can keep importing `tools.*` and `localpaths`.
        cast(Any, vendored_tools).localpaths = localpaths
        sys.modules["tools"] = vendored_tools
        sys.modules["tools.localpaths"] = localpaths
        sys.modules["localpaths"] = localpaths
        localpaths.repo_root = os.path.abspath(wpt_directory)

        from wptserve.handlers import FileHandler
        from wptserve.handlers import python_script_handler
        from wptserve.request import Request
        from wptserve.request import Server
        from wptserve.response import Response
        from wptserve.stash import Stash
        from wptserve.stash import start_server as start_stash_server
        from wptserve.utils import HTTPException

        cast(Any, Server).config = SimpleNamespace(logging={"suppress_handler_traceback": False})

        self.request_class = Request
        self.response_class = Response
        self.file_handler_class = FileHandler
        self.python_script_handler = python_script_handler
        self.http_exception = HTTPException
        self.stash_class = Stash
        self.stash_manager, self.stash_address, self.stash_authkey = start_stash_server()

    def close(self):
        self.stash_manager.shutdown()


class TestHTTPServer(socketserver.ThreadingTCPServer):
    scheme: str
    router: SimpleNamespace
    wpt: WPTContext
    wpt_file_handler: Callable[[Any, Any], Any]
    static_file_handler: Callable[[Any, Any], Any]


class TestHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    static_directory: str
    wpt_directory: str

    def __init__(self, *arguments, **kwargs):
        super().__init__(*arguments, directory=self.static_directory, **kwargs)

    def _test_server(self):
        return cast(TestHTTPServer, self.server)

    def end_headers(self):
        extra_headers = getattr(self, "_extra_headers", None)
        if extra_headers is not None:
            self._sending_extra_headers = True
            for key, value in extra_headers:
                self.send_header(key, value)
            self._sending_extra_headers = False
            delattr(self, "_extra_headers")
            delattr(self, "_extra_header_names")
        super().end_headers()

    def send_header(self, keyword, value):
        # Headers from .headers files override headers created by SimpleHTTPRequestHandler.
        extra_header_names = getattr(self, "_extra_header_names", None)
        if (
            extra_header_names is not None
            and not getattr(self, "_sending_extra_headers", False)
            and keyword.lower() in extra_header_names
        ):
            return
        super().send_header(keyword, value)

    def _request_target(self):
        if self.path.startswith("/static/"):
            # Explicit /static/ URLs continue to serve files from the general test root.
            return self.static_directory, self.path[7:]

        # All other non-echo URLs are served from the imported WPT tree.
        # This lets absolute WPT paths like /html/... resolve through the test server.
        return self.wpt_directory, self.path

    def _serve_wpt_python_script(self):
        self.directory, self.path = self._request_target()
        server = self._test_server()
        server.router.doc_root = self.directory

        request, response = self._create_wpt_request_response()

        try:
            server.wpt.python_script_handler(request, response)
        except server.wpt.http_exception as exception:
            response.set_error(exception.code, exception)
        except Exception as exception:
            response.set_error(500, exception)

        if not response.writer.content_written:
            response.write()
        if response.close_connection:
            self.close_connection = True
        request.close()

    def _create_wpt_request_response(self):
        server = self._test_server()
        request = server.wpt.request_class(self)
        request.server._stash = server.wpt.stash_class(
            request.url_parts.path,
            server.wpt.stash_address,
            server.wpt.stash_authkey,
        )
        response = server.wpt.response_class(self, request)
        return request, response

    def _serve_wpt_file_request(self):
        self.directory, _ = self._request_target()
        server = self._test_server()
        server.router.doc_root = self.directory

        request, response = self._create_wpt_request_response()

        if self.path.startswith("/static/"):
            handler = server.static_file_handler
        else:
            handler = server.wpt_file_handler

        try:
            handler(request, response)
        except server.wpt.http_exception as exception:
            response.set_error(exception.code, exception)
        except Exception as exception:
            response.set_error(500, exception)

        if not response.writer.content_written:
            response.write()
        if response.close_connection:
            self.close_connection = True
        request.close()

    def _serve_static_request(self):
        self._serve_wpt_file_request()

    def do_GET(self):
        if self.headers.get("Upgrade", "").lower() == "websocket":
            self._serve_websocket_echo()
            return

        request_path = self.path.partition("?")[0]

        if request_path.startswith("/unblock/"):
            self._serve_unblock()
        elif request_path.startswith("/echo"):
            self.handle_echo()
        elif request_path.startswith("/recorded-request-headers/"):
            self._serve_recorded_request_headers()
        elif request_path.endswith(".py"):
            self._serve_wpt_python_script()
        else:
            self._serve_static_request()

    def do_POST(self):
        request_path = self.path.partition("?")[0]

        if request_path == "/echo":
            self._register_echo()
        elif request_path.startswith("/echo/"):
            self.handle_echo()
        elif request_path.endswith(".py"):
            self._serve_wpt_python_script()
        else:
            self.send_error(405, "Method Not Allowed")

    def do_OPTIONS(self):
        request_path = self.path.partition("?")[0]

        if request_path.startswith("/echo"):
            # Requests with "credentials=include" cannot have "Access-Control-Allow-Origin=*". If the test registered
            # an OPTIONS echo, return the headers that it specified.
            key = f"OPTIONS {self.path}"
            if key in echo_store:
                self.handle_echo()
                return

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

    def _register_echo(self):
        """Handle a request to register an echo server handler"""
        content_length = int(self.headers["Content-Length"])
        post_data = self.rfile.read(content_length)
        data = json.loads(post_data.decode("utf-8"))

        echo = Echo()
        echo.method = data.get("method", None)
        echo.path = data.get("path", None)
        echo.status = data.get("status", None)
        echo.body = data.get("body", None)
        echo.body_encoding = data.get("body_encoding", "raw")
        echo.delay_ms = data.get("delay_ms", None)
        echo.headers = data.get("headers", {})
        echo.reason_phrase = data.get("reason_phrase", None)
        echo.reflect_headers_in_body = data.get("reflect_headers_in_body", False)
        echo.close_connection = data.get("close_connection", False)
        echo.wait_for_unblock = data.get("wait_for_unblock", None)

        is_invalid_echo_path = echo.path is None or not echo.path.startswith("/echo/")

        # Return 400: Bad Request if invalid params are given or a reserved path is given
        if (
            echo.method is None
            or echo.path is None
            or echo.status is None
            or echo.body_encoding not in ("raw", "base64")
            or (echo.body is not None and "$HEADERS" not in echo.body and echo.reflect_headers_in_body)
            or is_invalid_echo_path
        ):
            self.send_response(400)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            return

        # Return 409: Conflict if the method+path combination already exists
        key = f"{echo.method} {urllib.parse.urlparse(echo.path).path}"
        if key in echo_store and echo_store[key] != echo:
            self.send_response(409)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            message = (
                "Echo already exists for method+path, but with a different definition.\n"
                f"key: {key}\n"
                "Hint: Use a unique path per test run (or keep the same definition).\n"
            )
            self.wfile.write(message.encode("utf-8"))
            return

        echo_store[key] = echo
        if echo.wait_for_unblock is not None:
            unblock_events[echo.wait_for_unblock] = threading.Event()

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

    def _serve_unblock(self):
        token = urllib.parse.unquote(self.path.partition("?")[0][len("/unblock/") :])
        event = unblock_events.setdefault(token, threading.Event())
        event.set()
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()

    def _serve_recorded_request_headers(self):
        echo_path = self.path[len("/recorded-request-headers") :]
        headers = recorded_request_headers.get(echo_path)
        if headers is None:
            self.send_error(404, f"No recorded request at {echo_path}")
            return
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(headers).encode("utf-8"))

    def handle_echo(self):
        method = self.command.upper()
        parsed_url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed_url.query)
        key = f"{method} {self.path}"
        if key not in echo_store:
            key = f"{method} {parsed_url.path}"

        headers_for_path: Dict[str, list[str]] = {}
        for header, value in self.headers.items():
            headers_for_path.setdefault(header, []).append(value)
        recorded_request_headers[self.path] = headers_for_path

        if parsed_url.path != self.path:
            recorded_request_headers[parsed_url.path] = recorded_request_headers[self.path]

        is_revalidation_request = "If-Modified-Since" in self.headers
        send_not_modified = is_revalidation_request and "X-Ladybird-Respond-With-Not-Modified" in self.headers

        send_incomplete_response = "X-Ladybird-Respond-With-Incomplete-Response" in self.headers

        set_invalid_cookie = "X-Ladybird-Set-Invalid-Cookie" in self.headers

        if key not in echo_store:
            self.send_error(404, f"Echo response not found for {key}")
            return

        echo = echo_store[key]

        if echo.close_connection:
            self.connection.shutdown(socket.SHUT_WR)
            self.connection.close()
            return

        if echo.wait_for_unblock is not None:
            event = unblock_events.setdefault(echo.wait_for_unblock, threading.Event())
            event.wait()

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
            headers = {}
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

        if echo.body_encoding == "base64":
            response_body_bytes = base64.b64decode(response_body)
        else:
            response_body_bytes = response_body.encode("utf-8")

        chunks = query.get("chunks", [])
        chunk_delay_ms = int(query.get("chunk_delay_ms", [0])[0] or 0)
        if chunks:
            chunk_sizes = [int(chunk_size) for chunk_size in chunks[0].split(",") if chunk_size]
            offset = 0
            for chunk_size in chunk_sizes:
                self.wfile.write(response_body_bytes[offset : offset + chunk_size])
                self.wfile.flush()
                offset += chunk_size
                if chunk_delay_ms > 0:
                    time.sleep(chunk_delay_ms / 1000)
            if offset < len(response_body_bytes):
                self.wfile.write(response_body_bytes[offset:])
            return

        self.wfile.write(response_body_bytes)

    def do_other(self):
        request_path = self.path.partition("?")[0]

        if request_path.startswith("/echo"):
            self.handle_echo()
        elif request_path.endswith(".py"):
            self._serve_wpt_python_script()
        else:
            self.send_error(405, "Method Not Allowed")

    # --- Minimal RFC 6455 WebSocket echo --------------------------------------------------------------
    # Reached when a request carries an "Upgrade: websocket" header. The tests use this in place of an
    # externally-hosted echo server, so they have no network dependency. The client sends first; every
    # text/binary frame it sends is echoed back verbatim.

    def _serve_websocket_echo(self):
        key = self.headers.get("Sec-WebSocket-Key")
        if not key:
            self.send_error(400, "Missing Sec-WebSocket-Key")
            return

        accept = base64.b64encode(hashlib.sha1((key + WEBSOCKET_ACCEPT_GUID).encode("utf-8")).digest()).decode("ascii")
        handshake = (
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Accept: {accept}\r\n"
            "\r\n"
        )
        # We now own the raw socket; stop the HTTP handler from parsing another request on it.
        self.close_connection = True
        self.connection.sendall(handshake.encode("ascii"))

        while True:
            frame = self._read_websocket_frame()
            if frame is None:
                break
            opcode, payload = frame
            if opcode == 0x8:  # Close
                self._send_websocket_close()
                break
            if opcode == 0x9:  # Ping -> Pong
                self._send_websocket_frame(payload, opcode=0xA)
                continue
            if opcode in (0x1, 0x2):  # Text / Binary -> echo verbatim
                self._send_websocket_frame(payload, opcode=opcode)

    def _recv_exact(self, count):
        data = bytearray()
        while len(data) < count:
            chunk = self.rfile.read(count - len(data))
            if not chunk:
                return None
            data += chunk
        return bytes(data)

    def _read_websocket_frame(self):
        header = self._recv_exact(2)
        if header is None:
            return None

        opcode = header[0] & 0x0F
        is_masked = (header[1] & 0x80) != 0
        length = header[1] & 0x7F
        if length == 126:
            extended = self._recv_exact(2)
            if extended is None:
                return None
            length = int.from_bytes(extended, "big")
        elif length == 127:
            extended = self._recv_exact(8)
            if extended is None:
                return None
            length = int.from_bytes(extended, "big")

        mask = b""
        if is_masked:
            mask = self._recv_exact(4)
            if mask is None:
                return None

        payload = self._recv_exact(length) if length else b""
        if payload is None:
            return None
        if is_masked:
            payload = bytes(payload[i] ^ mask[i % 4] for i in range(length))
        return opcode, payload

    def _build_ws_frame(self, data, opcode=0x1):
        header = bytearray([0x80 | opcode])
        length = len(data)
        if length < 126:
            header.append(length)
        elif length < 65536:
            header.append(126)
            header += length.to_bytes(2, "big")
        else:
            header.append(127)
            header += length.to_bytes(8, "big")
        return bytes(header) + data

    def _send_websocket_frame(self, data, opcode=0x1):
        if isinstance(data, str):
            data = data.encode("utf-8")
        try:
            self.connection.sendall(self._build_ws_frame(data, opcode))
        except OSError:
            pass

    def _send_websocket_close(self):
        try:
            self.connection.sendall(bytes([0x88, 0x00]))  # FIN + Close opcode, empty payload.
        except OSError:
            pass


def start_server(port, static_directory):
    TestHTTPRequestHandler.static_directory = os.path.abspath(static_directory)
    TestHTTPRequestHandler.wpt_directory = os.path.join(
        TestHTTPRequestHandler.static_directory, "Text", "input", "wpt-import"
    )
    httpd = TestHTTPServer(("127.0.0.1", port), TestHTTPRequestHandler)
    httpd.daemon_threads = True
    httpd.scheme = "http"
    httpd.router = SimpleNamespace(doc_root=TestHTTPRequestHandler.wpt_directory)
    httpd.wpt = WPTContext(TestHTTPRequestHandler.wpt_directory)
    httpd.wpt_file_handler = httpd.wpt.file_handler_class(base_path=TestHTTPRequestHandler.wpt_directory)
    httpd.static_file_handler = httpd.wpt.file_handler_class(
        base_path=TestHTTPRequestHandler.static_directory,
        url_base="/static/",
    )

    print(httpd.socket.getsockname()[1])
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.wpt.close()
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
