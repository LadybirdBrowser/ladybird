#!/usr/bin/env python3

import argparse
import http.server
import json
import os
import signal
import socketserver
import sys
import threading
import time

from typing import Dict
from typing import List
from typing import Optional

"""
Multi-Origin HTTP Test Server for security/origin confusion testing.

Starts multiple HTTP servers on different ports, each representing a distinct origin.
Supports virtual host routing for hostname-based origin testing.

Endpoints (per server):
    - GET /static/<path>   : Serve static files
    - POST /echo           : Create dynamic response endpoints
    - GET/POST/etc /<path> : Serve previously registered echo responses

Output format (JSON to stdout):
    {"origins": [{"port": 8001, "host": "127.0.0.1"}, ...]}
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

    def __eq__(self, other):
        if not isinstance(other, Echo):
            return NotImplemented
        return (
            self.method == other.method
            and self.path == other.path
            and self.status == other.status
            and self.body == other.body
            and self.delay_ms == other.delay_ms
            and self.headers == other.headers
            and self.reason_phrase == other.reason_phrase
            and self.reflect_headers_in_body == other.reflect_headers_in_body
        )


# Shared echo store across all origins (keyed by origin+method+path)
echo_store: Dict[str, Echo] = {}
echo_store_lock = threading.Lock()


class MultiOriginRequestHandler(http.server.SimpleHTTPRequestHandler):
    static_directory: str
    origin_index: int  # Which origin this handler belongs to

    def __init__(self, *args, origin_index: int = 0, **kwargs):
        self.origin_index = origin_index
        super().__init__(*args, **kwargs)

    def end_headers(self):
        if hasattr(self, "_extra_headers"):
            for key, value in self._extra_headers:
                self.send_header(key, value)
            del self._extra_headers
        super().end_headers()

    def log_message(self, format, *args):
        # Prefix logs with origin index for debugging
        sys.stderr.write(f"[origin-{self.origin_index}] {self.address_string()} - {format % args}\n")

    def _get_origin_key(self, method: str, path: str) -> str:
        """Generate a unique key for this origin's echo store."""
        port = self.server.server_address[1]
        return f"{port}:{method} {path}"

    def do_GET(self):
        if self.path.startswith("/static/"):
            self.path = self.path[7:]
            file_path = self.translate_path(self.path)
            headers_path = file_path + ".headers"
            if os.path.isfile(headers_path):
                self._extra_headers = []
                with open(headers_path) as f:
                    for line in f:
                        line = line.strip()
                        if ":" in line:
                            key, _, value = line.partition(":")
                            self._extra_headers.append((key.strip(), value.strip()))
            return super().do_GET()
        elif self.path == "/info":
            self._handle_info()
        else:
            self._handle_echo()

    def do_POST(self):
        if self.path == "/echo":
            self._create_echo()
        elif self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self._handle_echo()

    def do_OPTIONS(self):
        if self.path.startswith("/echo") or self.path == "/info":
            self.send_response(204)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "*")
            self.send_header("Access-Control-Allow-Headers", "*")
            self.end_headers()
        else:
            self._handle_echo()

    def do_PUT(self):
        self._handle_echo()

    def do_HEAD(self):
        self._handle_echo()

    def do_DELETE(self):
        self._handle_echo()

    def _handle_info(self):
        """Return information about this origin."""
        port = self.server.server_address[1]
        info = {
            "origin_index": self.origin_index,
            "port": port,
            "host": "127.0.0.1",
            "origin": f"http://127.0.0.1:{port}",
        }
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps(info).encode("utf-8"))

    def _create_echo(self):
        """Create a new echo endpoint for this origin."""
        content_length = int(self.headers.get("Content-Length", 0))
        post_data = self.rfile.read(content_length)
        data = json.loads(post_data.decode("utf-8"))

        echo = Echo()
        echo.method = data.get("method")
        echo.path = data.get("path")
        echo.status = data.get("status")
        echo.body = data.get("body")
        echo.delay_ms = data.get("delay_ms")
        echo.headers = data.get("headers", {})
        echo.reason_phrase = data.get("reason_phrase")
        echo.reflect_headers_in_body = data.get("reflect_headers_in_body", False)

        is_reserved = echo.path and (echo.path.startswith("/static") or echo.path.startswith("/echo") or echo.path == "/info")

        if (
            echo.method is None
            or echo.path is None
            or echo.status is None
            or (echo.body is not None and "$HEADERS" not in echo.body and echo.reflect_headers_in_body)
            or is_reserved
        ):
            self.send_response(400)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            return

        key = self._get_origin_key(echo.method, echo.path)

        with echo_store_lock:
            if key in echo_store and echo_store[key] != echo:
                self.send_response(409)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(f"Echo conflict for {key}\n".encode("utf-8"))
                return
            echo_store[key] = echo

        port = self.server.server_address[1]
        fetch_url = f"http://127.0.0.1:{port}{echo.path}"
        fetch_config = {"method": echo.method, "url": fetch_url}

        self.send_response(201)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Content-Type", "application/json")
        self.end_headers()
        self.wfile.write(json.dumps(fetch_config).encode("utf-8"))

    def _handle_echo(self):
        """Handle requests to echo endpoints."""
        method = self.command.upper()
        key = self._get_origin_key(method, self.path)

        with echo_store_lock:
            if key not in echo_store:
                self.send_error(404, f"Echo not found for {key}")
                return
            echo = echo_store[key]

        if echo.delay_ms:
            time.sleep(echo.delay_ms / 1000)

        self.send_response_only(echo.status, echo.reason_phrase)

        response_headers = echo.headers.copy()
        for header, value in response_headers.items():
            self.send_header(header, value)
        self.end_headers()

        if echo.reflect_headers_in_body:
            headers = {k: self.headers.get_all(k) for k in self.headers.keys()}
            headers_json = json.dumps(headers)
            response_body = echo.body.replace("$HEADERS", headers_json) if echo.body else headers_json
        else:
            response_body = echo.body or ""

        self.wfile.write(response_body.encode("utf-8"))


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True


def create_handler_class(static_directory: str, origin_index: int):
    """Create a handler class with the given static directory and origin index."""
    class ConfiguredHandler(MultiOriginRequestHandler):
        pass
    ConfiguredHandler.static_directory = os.path.abspath(static_directory)
    ConfiguredHandler.origin_index = origin_index
    
    # Override __init__ to pass origin_index
    def patched_init(self, *args, **kwargs):
        kwargs['origin_index'] = origin_index
        kwargs['directory'] = os.path.abspath(static_directory)
        http.server.SimpleHTTPRequestHandler.__init__(self, *args, **kwargs)
        self.origin_index = origin_index
    ConfiguredHandler.__init__ = patched_init
    
    return ConfiguredHandler


def start_servers(num_origins: int, static_directory: str, base_port: int = 0) -> List[ThreadedTCPServer]:
    """Start multiple HTTP servers on different ports."""
    servers = []
    
    for i in range(num_origins):
        handler_class = create_handler_class(static_directory, i)
        port = base_port + i if base_port > 0 else 0
        
        server = ThreadedTCPServer(("127.0.0.1", port), handler_class)
        servers.append(server)
    
    return servers


def main():
    parser = argparse.ArgumentParser(description="Multi-Origin HTTP Test Server")
    parser.add_argument(
        "-n", "--num-origins",
        type=int,
        default=3,
        help="Number of distinct origins (servers) to start (default: 3)",
    )
    parser.add_argument(
        "-d", "--directory",
        type=str,
        default=".",
        help="Directory to serve static files from",
    )
    parser.add_argument(
        "-p", "--base-port",
        type=int,
        default=0,
        help="Base port (0 for auto-assign). Origins will use base, base+1, base+2, etc.",
    )
    args = parser.parse_args()

    servers = start_servers(args.num_origins, args.directory, args.base_port)
    
    # Start server threads
    threads = []
    for server in servers:
        thread = threading.Thread(target=server.serve_forever)
        thread.daemon = True
        thread.start()
        threads.append(thread)

    # Output server info as JSON (first line for parsing)
    origins = []
    for i, server in enumerate(servers):
        port = server.server_address[1]
        origins.append({
            "index": i,
            "port": port,
            "host": "127.0.0.1",
            "origin": f"http://127.0.0.1:{port}",
        })
    
    output = json.dumps({"origins": origins})
    print(output)
    sys.stdout.flush()

    # Log to stderr for debugging
    sys.stderr.write(f"Multi-origin server started with {len(origins)} origins:\n")
    for origin in origins:
        sys.stderr.write(f"  Origin {origin['index']}: {origin['origin']}\n")
    sys.stderr.flush()

    # Handle shutdown gracefully
    def shutdown_handler(signum, frame):
        sys.stderr.write("\nShutting down servers...\n")
        for server in servers:
            server.shutdown()
        sys.exit(0)

    signal.signal(signal.SIGINT, shutdown_handler)
    signal.signal(signal.SIGTERM, shutdown_handler)

    # Keep main thread alive
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        shutdown_handler(None, None)


if __name__ == "__main__":
    main()
