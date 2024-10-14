#!/usr/bin/env python3
import http.client
import http.server
import json
import os
import signal
import socketserver
import subprocess
import argparse
import sys
import time

# In-memory store for echo responses
echo_store = {}


class TestHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=self.static_directory, **kwargs)

    def do_GET(self):
        if self.path == "/shutdown":
            self.send_response(200)
            self.send_header("Content-type", "text/html")
            self.end_headers()
            self.wfile.write(b"Goodbye")
            self.server.server_close()
            print("Goodbye")
            sys.exit(0)
        elif self.path == "/ping":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"pong")
        elif self.path.startswith("/static/"):
            # Remove '/static/' prefix and use built-in method
            self.path = self.path[7:]
            return super().do_GET()
        else:
            self.handle_echo()

    def do_POST(self):
        if self.path == "/create":
            content_length = int(self.headers["Content-Length"])
            post_data = self.rfile.read(content_length)
            response_def = json.loads(post_data.decode("utf-8"))

            method = response_def.get("method", "GET").upper()
            path = response_def.get("path", "")
            key = f'{method} {path}'

            is_invalid_path = path.startswith('/static') or path == '/create' or path == '/shutdown' or path == '/ping'
            if (is_invalid_path or key in echo_store):
                self.send_response(400)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()

                if is_invalid_path:
                    self.wfile.write(b"invalid path, must not be /static, /create, /shutdown, /ping")
                else:
                    self.wfile.write(b"invalid path, already registered")

                return

            echo_store[key] = response_def

            host = self.headers.get('host', 'localhost')
            path = path.lstrip('/')
            fetch_url = f'http://{host}/{path}'

            # The params to use on the client when making a request to the newly created echo endpoint
            fetch_config = {
                "method": method,
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
        if self.path.startswith("/create"):
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
        key = f'{method} {self.path}'

        if key in echo_store:
            response_def = echo_store[key]

            if "delay" in response_def:
                time.sleep(response_def["delay"])

            # Send the status code without any default headers
            self.send_response_only(response_def.get("status", 200))

            # Set only the headers defined in the echo definition
            for header, value in response_def.get("headers", {}).items():
                self.send_header(header, value)
            self.end_headers()

            self.wfile.write(response_def.get("body", "").encode("utf-8"))
        else:
            self.send_error(404, f"Echo response not found for {key}")

    def do_other(self):
        if self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self.handle_echo()


pid_file_path = "http-test-server.pid.txt"
log_file_path = "http-test-server.log"


def run_server(port=8000, static_directory="."):
    TestHTTPRequestHandler.static_directory = os.path.abspath(static_directory)
    httpd = socketserver.TCPServer(("", port), TestHTTPRequestHandler)

    print(f"Serving at http://localhost:{port}/")
    print(
        f"Serving static files from directory: {TestHTTPRequestHandler.static_directory}"
    )

    # Save pid to file
    with open(pid_file_path, "w") as f:
        f.write(str(os.getpid()))

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        httpd.server_close()

    print("Goodbye")


def stop_server(quiet=False):
    if os.path.exists(pid_file_path):
        with open(pid_file_path, "r") as f:
            pid = int(f.read().strip())
        try:
            os.kill(pid, signal.SIGTERM)
            os.remove(pid_file_path)
            print("Server stopped")
        except ProcessLookupError:
            print("Server not running")
        except PermissionError:
            print("Permission denied when trying to stop the server")
    elif not quiet:
        print("No server running")


def start_server_in_background(port, directory):
    # Launch the server as a detached subprocess
    with open(log_file_path, "w") as log_file:
        stop_server(True)
        subprocess.Popen(
            [sys.executable, __file__, "start", "-p", str(port), "-d", directory],
            stdout=log_file,
            stderr=log_file,
            preexec_fn=os.setpgrp,
        )

    # Sleep to give the server time to start
    time.sleep(0.05)

    # Verify that the server is up by sending a GET request to /ping
    max_retries = 3
    for i in range(max_retries):
        try:
            conn = http.client.HTTPConnection("localhost", port, timeout=1)
            conn.request("GET", "/ping")
            response = conn.getresponse()
            if response.status == 200 and response.read().decode().strip() == "pong":
                print(f"Server successfully started on port {port}")
                return True
        except (http.client.HTTPException, ConnectionRefusedError, OSError):
            if i < max_retries - 1:
                print(
                    f"Server not ready, retrying in 1 second... (Attempt {i+1}/{max_retries})"
                )
                time.sleep(1)
            else:
                print(f"Failed to start server after {max_retries} attempts")
                return False
        finally:
            conn.close()

    print(f"Server verification failed after {max_retries} attempts")
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run a test HTTP server")
    parser.add_argument(
        "-p", "--port", type=int, default=8123, help="Port to run the server on"
    )
    parser.add_argument(
        "-d",
        "--directory",
        type=str,
        default=".",
        help="Directory to serve static files from",
    )
    parser.add_argument(
        "-b",
        "--background",
        action="store_true",
        help="Run the server in the background",
    )
    parser.add_argument("action", choices=["start", "stop"], help="Action to perform")
    args = parser.parse_args()

    if args.action == "start":
        if args.background:
            # Detach the server and run in the background
            start_server_in_background(args.port, args.directory)
            print(f"Server started in the background, check '{log_file_path}' for details.")
        else:
            # Run normally
            run_server(port=args.port, static_directory=args.directory)
    elif args.action == "stop":
        stop_server()
