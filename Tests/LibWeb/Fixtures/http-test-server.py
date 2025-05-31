#!/usr/bin/env python3
import argparse
import base64
import hashlib
import http.server
import json
import os
import socketserver
import struct
import sys
import threading
import time
from typing import Dict, Optional

"""
Description:
    This script starts a simple HTTP echo server with WebSocket support on localhost for use in our in-tree tests.
    The port is assigned by the OS on startup and printed to stdout.

HTTP Endpoints:
    - POST /echo <json body>, Creates an echo response for later use. See "Echo" class below for body properties.
    - Static file serving from /static/ path

WebSocket:
    - WebSocket connections are available at ws://localhost:port/
    - Echoes back any text or binary messages received
"""


class Echo:
    method: str
    path: str
    status: int
    headers: Optional[Dict[str, str]]
    body: Optional[str]
    delay_ms: Optional[int]
    reason_phrase: Optional[str]


# In-memory store for echo responses
echo_store: Dict[str, Echo] = {}


class WebSocketHandler:
    WEBSOCKET_MAGIC_STRING = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

    def __init__(self, request_handler):
        self.request_handler = request_handler

    def is_websocket_request(self):
        """Check if the current request is a WebSocket upgrade request"""
        headers = self.request_handler.headers
        return (
            headers.get("upgrade", "").lower() == "websocket"
            and headers.get("connection", "").lower() == "upgrade"
            and "sec-websocket-key" in headers
        )

    def perform_handshake(self):
        """Perform WebSocket handshake"""
        try:
            websocket_key = self.request_handler.headers["sec-websocket-key"]
            accept_key = self.generate_accept_key(websocket_key)

            # Send handshake response
            response = (
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                f"Sec-WebSocket-Accept: {accept_key}\r\n"
                "\r\n"
            )
            self.request_handler.wfile.write(response.encode("utf-8"))
            self.request_handler.wfile.flush()
            return True

        except Exception as e:
            print(f"WebSocket handshake error: {e}")
            return False

    def generate_accept_key(self, websocket_key):
        """Generate WebSocket accept key"""
        combined = websocket_key + self.WEBSOCKET_MAGIC_STRING
        sha1_hash = hashlib.sha1(combined.encode("utf-8")).digest()
        return base64.b64encode(sha1_hash).decode("utf-8")

    def handle_websocket_connection(self):
        """Handle WebSocket connection after successful handshake"""
        client_socket = self.request_handler.connection

        print("WebSocket connection established")

        # Send initial greeting
        self.send_text_frame(client_socket, "WebSocket Echo Server Connected")

        try:
            while True:
                # Read frame header
                header = client_socket.recv(2)
                if len(header) < 2:
                    break

                byte1, byte2 = struct.unpack("!BB", header)

                # Parse frame header
                opcode = byte1 & 0x0F
                masked = bool(byte2 & 0x80)
                payload_length = byte2 & 0x7F

                # Handle different payload lengths
                if payload_length == 126:
                    length_data = client_socket.recv(2)
                    if len(length_data) < 2:
                        break
                    payload_length = struct.unpack("!H", length_data)[0]
                elif payload_length == 127:
                    length_data = client_socket.recv(8)
                    if len(length_data) < 8:
                        break
                    payload_length = struct.unpack("!Q", length_data)[0]

                # Read masking key if present
                mask_key = None
                if masked:
                    mask_key = client_socket.recv(4)
                    if len(mask_key) < 4:
                        break

                # Read payload
                payload = client_socket.recv(payload_length)
                if len(payload) < payload_length:
                    break

                # Unmask payload if necessary
                if masked and mask_key:
                    payload = bytes(payload[i] ^ mask_key[i % 4] for i in range(len(payload)))

                # Handle different frame types
                if opcode == 0x1:  # Text frame
                    message = payload.decode("utf-8")
                    print(f"WebSocket received: {message}")
                    # Echo the message back
                    self.send_text_frame(client_socket, f"{message}")
                elif opcode == 0x2:  # Binary frame
                    print(f"WebSocket received binary data: {len(payload)} bytes")
                    # Echo the binary data back
                    self.send_binary_frame(client_socket, payload)
                elif opcode == 0x8:  # Close frame
                    print("WebSocket close frame received")
                    self.send_close_frame(client_socket)
                    break
                elif opcode == 0x9:  # Ping frame
                    print("WebSocket ping frame received")
                    self.send_pong_frame(client_socket, payload)
                elif opcode == 0xA:  # Pong frame
                    print("WebSocket pong frame received")

        except Exception as e:
            print(f"WebSocket error: {e}")
        finally:
            print("WebSocket connection closed")

    def send_frame(self, client_socket, opcode, payload):
        """Send a WebSocket frame"""
        payload_length = len(payload)

        # Create frame header
        frame = bytearray()
        frame.append(0x80 | opcode)  # FIN=1, opcode

        # Add payload length
        if payload_length < 126:
            frame.append(payload_length)
        elif payload_length < 65536:
            frame.append(126)
            frame.extend(struct.pack("!H", payload_length))
        else:
            frame.append(127)
            frame.extend(struct.pack("!Q", payload_length))

        # Add payload
        frame.extend(payload)

        client_socket.send(bytes(frame))

    def send_text_frame(self, client_socket, text):
        """Send a text frame"""
        self.send_frame(client_socket, 0x1, text.encode("utf-8"))

    def send_binary_frame(self, client_socket, data):
        """Send a binary frame"""
        self.send_frame(client_socket, 0x2, data)

    def send_close_frame(self, client_socket):
        """Send a close frame"""
        self.send_frame(client_socket, 0x8, b"")

    def send_pong_frame(self, client_socket, payload):
        """Send a pong frame in response to ping"""
        self.send_frame(client_socket, 0xA, payload)


class IntegratedHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *arguments, **kwargs):
        super().__init__(*arguments, directory=None, **kwargs)

    def do_GET(self):
        # Check if this is a WebSocket upgrade request
        ws_handler = WebSocketHandler(self)
        if ws_handler.is_websocket_request():
            if ws_handler.perform_handshake():
                # Handle WebSocket connection in a separate thread to avoid blocking
                ws_thread = threading.Thread(target=ws_handler.handle_websocket_connection)
                ws_thread.daemon = True
                ws_thread.start()
                # Keep the connection alive for WebSocket
                ws_thread.join()
            return

        # Handle regular HTTP GET requests
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
            echo.headers = data.get("headers", None)
            echo.reason_phrase = data.get("reason_phrase", None)

            is_using_reserved_path = echo.path.startswith("/static") or echo.path.startswith("/echo")

            # Return 400: Bad Request if invalid params are given or a reserved path is given
            if echo.method is None or echo.path is None or echo.status is None or is_using_reserved_path:
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

        if key in echo_store:
            echo = echo_store[key]

            if echo.delay_ms is not None:
                time.sleep(echo.delay_ms / 1000)

            # Send the status code without any default headers
            self.send_response_only(echo.status, echo.reason_phrase)

            # Set only the headers defined in the echo definition
            if echo.headers is not None:
                for header, value in echo.headers.items():
                    self.send_header(header, value)
            self.end_headers()

            response_body = echo.body or ""
            self.wfile.write(response_body.encode("utf-8"))
        else:
            self.send_error(404, f"Echo response not found for {key}")

    def do_other(self):
        if self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self.handle_echo()


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    """Threaded TCP server to handle multiple connections simultaneously"""

    allow_reuse_address = True


def start_server(port, static_directory):
    IntegratedHTTPRequestHandler.static_directory = os.path.abspath(static_directory)
    httpd = ThreadedTCPServer(("127.0.0.1", port), IntegratedHTTPRequestHandler)

    actual_port = httpd.socket.getsockname()[1]
    print(actual_port)
    print(f"Server started on http://127.0.0.1:{actual_port}")
    print(f"WebSocket available at ws://127.0.0.1:{actual_port}/")
    print(f"Static files served from: {static_directory}")
    sys.stdout.flush()

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run an integrated HTTP and WebSocket echo server")
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
