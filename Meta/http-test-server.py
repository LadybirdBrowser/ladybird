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
from urllib.parse import urlparse

# In-memory store for echo responses
echo_store = {}


class TestHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=self.static_directory, **kwargs)

    def do_GET(self):
        if self.path == "/":
            self.send_response(200)
            self.send_header("Content-type", "text/html")
            self.end_headers()
            self.wfile.write(self.get_root_html().encode("utf-8"))
        elif self.path == "/shutdown":
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
        elif self.path.startswith("/echo/"):
            self.handle_echo()
        elif self.path.startswith("/static/"):
            # Remove '/static/' prefix and use built-in method
            self.path = self.path[7:]
            return super().do_GET()
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        if self.path == "/create":
            content_length = int(self.headers["Content-Length"])
            post_data = self.rfile.read(content_length)
            response_def = json.loads(post_data.decode("utf-8"))

            response_id = str(len(echo_store) + 1)
            echo_store[response_id] = response_def

            self.send_response(201)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({"id": response_id}).encode("utf-8"))
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
        parsed_path = urlparse(self.path)
        response_id = parsed_path.path.split("/")[-1]

        if response_id in echo_store:
            response_def = echo_store[response_id]

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
            self.send_error(404, "Echo response not found")

    def do_other(self):
        if self.path.startswith("/echo/"):
            self.handle_echo()
        elif self.path.startswith("/static/"):
            self.send_error(405, "Method Not Allowed")
        else:
            self.send_error(404, "Not Found")

    def get_root_html(self):
        return f"""
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>HTTP Test Server</title>
    <script src="https://cdn.tailwindcss.com"></script>
</head>
<body class="bg-gray-100 p-8">
    <div class="max-w-3xl mx-auto bg-white p-8 rounded-lg shadow-md">
        <h1 class="text-3xl font-bold mb-6 text-gray-800">HTTP Test Server</h1>

        <section class="mb-8">
            <h2 class="text-2xl font-semibold mb-4 text-gray-700">Available Endpoints:</h2>
            <ul class="list-disc pl-6 space-y-2 text-gray-600">
                <li><code class="bg-gray-200 px-2 py-1 rounded">/ping</code> - Simple health check (GET)</li>
                <li><code class="bg-gray-200 px-2 py-1 rounded">/create</code> - Create a new echo response (POST)</li>
                <li>
                    <code class="bg-gray-200 px-2 py-1 rounded">/echo/{{id}}</code>
                    - Get a specific echo response (GET, POST, PUT, OPTIONS, HEAD, DELETE)
                </li>
                <li><code class="bg-gray-200 px-2 py-1 rounded">/static/{{path}}</code> - Serve static files (GET)</li>
            </ul>
        </section>

        <section class="mb-8">
            <h2 class="text-2xl font-semibold mb-4 text-gray-700">Echo Definition Format:</h2>
            <pre class="bg-gray-100 p-4 rounded overflow-x-auto text-sm">
{{
    "status": int,
    "headers": {{
        "Header-Name": "Header-Value"
    }},
    "body": str,
    "delay": float  # Optional number of seconds
}}
            </pre>
        </section>

        <section class="mb-8">
            <h2 class="text-2xl font-semibold mb-4 text-gray-700">Example Usage (cURL):</h2>
            <p class="mb-2 text-gray-600">Create a new echo response:</p>
            <pre class="bg-gray-100 p-4 rounded overflow-x-auto text-sm mb-4">
curl -X POST http://localhost:{self.server.server_address[1]}/create -H "Content-Type: application/json" -d '{{
    "status": 200,
    "headers": {{
        "Content-Type": "application/json",
        "X-Custom-Header": "Custom Value"
    }},
    "body": "{{\\"message\\": \\"Hello, World!\\"}}",
    "delay": 1.5
}}'
            </pre>
            <p class="mb-2 text-gray-600">Access the created echo response:</p>
            <pre class="bg-gray-100 p-4 rounded overflow-x-auto text-sm">
curl http://localhost:{self.server.server_address[1]}/echo/1
            </pre>
        </section>

        <section class="mb-8">
            <h2 class="text-2xl font-semibold mb-4 text-gray-700">Example Usage (Browser JavaScript):</h2>
            <pre class="bg-gray-100 p-4 rounded overflow-x-auto text-sm mb-4">
// Create a new echo response
async function createEchoResponse() {{
    const response = await fetch('http://localhost:{self.server.server_address[1]}/create', {{
        method: 'POST',
        headers: {{
            'Content-Type': 'application/json',
        }},
        body: JSON.stringify({{
            status: 200,
            headers: {{
                'Content-Type': 'application/json',
                'X-Custom-Header': 'Custom Value'
            }},
            body: JSON.stringify({{ message: 'Hello from browser!' }}),
            delay: 1
        }}),
    }});
    const data = await response.json();
    console.log('Created echo response with ID:', data.id);
    return data.id;
}}

// Access the created echo response
async function accessEchoResponse(id) {{
    const response = await fetch(`http://localhost:{self.server.server_address[1]}/echo/${{id}}`);
    const data = await response.json();
    console.log('Received echo response:', data);
}}

// Usage
createEchoResponse().then(id => accessEchoResponse(id));
            </pre>
        </section>
    </div>
</body>
</html>
    """


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
        "-p", "--port", type=int, default=8000, help="Port to run the server on"
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
