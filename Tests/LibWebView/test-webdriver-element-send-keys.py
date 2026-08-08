#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.client
import http.server
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

EVENT_TIMEOUT_SECONDS = 30
WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 60

TEST_PAGE = b"""<!doctype html>
<title>Element Send Keys</title>
<input id="file" type="file">
<input id="file-multiple" type="file" multiple>
<input id="color" type="color">
<p>Element Send Keys</p>"""


class TestPageHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/send-keys":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(TEST_PAGE)
            return

        self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"not found")

    def log_message(self, format, *args):
        pass


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, timeout=EVENT_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for port {port}")


def request_raw(webdriver_port, method, path, body=None):
    encoded_body = None
    headers = {}
    if body is not None:
        encoded_body = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"

    connection = http.client.HTTPConnection("127.0.0.1", webdriver_port, timeout=WEBDRIVER_REQUEST_TIMEOUT_SECONDS)
    try:
        connection.request(method, path, encoded_body, headers)
        response = connection.getresponse()
        response_body = response.read().decode()
    finally:
        connection.close()

    try:
        payload = json.loads(response_body) if response_body else {}
    except json.JSONDecodeError as error:
        raise RuntimeError(f"{method} {path} returned invalid JSON: {response_body}") from error

    return response.status, payload, response_body


def request(webdriver_port, method, path, body=None):
    status, payload, response_body = request_raw(webdriver_port, method, path, body)
    if status >= 400 or isinstance(payload.get("value"), dict) and payload["value"].get("error"):
        raise RuntimeError(f"{method} {path} failed with HTTP {status}: {response_body}")

    return payload


def execute_script(webdriver_port, session_id, script, args=None):
    return request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/execute/sync",
        {
            "script": script,
            "args": args or [],
        },
    )["value"]


def find_element(webdriver_port, session_id, css_selector):
    value = request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/element",
        {"using": "css selector", "value": css_selector},
    )["value"]
    return next(iter(value.values()))


def element_send_keys(webdriver_port, session_id, element_id, text):
    return request_raw(
        webdriver_port,
        "POST",
        f"/session/{session_id}/element/{element_id}/value",
        {"text": text},
    )


def expect_send_keys_success(webdriver_port, session_id, element_id, text, label):
    status, payload, response_body = element_send_keys(webdriver_port, session_id, element_id, text)
    value = payload.get("value")
    if status != 200 or (isinstance(value, dict) and value.get("error")):
        raise AssertionError(f"Expected {label} to succeed, got HTTP {status}: {response_body}")


def expect_send_keys_error(webdriver_port, session_id, element_id, text, expected_error, label):
    status, payload, response_body = element_send_keys(webdriver_port, session_id, element_id, text)
    value = payload.get("value")
    actual_error = value.get("error") if isinstance(value, dict) else None
    if status < 400 or actual_error != expected_error:
        raise AssertionError(f"Expected {label} to fail with {expected_error}, got HTTP {status}: {response_body}")


def create_session(webdriver_port):
    created = request(
        webdriver_port,
        "POST",
        "/session",
        {"capabilities": {"alwaysMatch": {"ladybird:headless": True, "pageLoadStrategy": "normal"}}},
    )
    session_id = created.get("value", {}).get("sessionId") or created.get("sessionId")
    if not session_id:
        raise RuntimeError(f"Could not find session id in response: {created}")

    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
    return session_id


def selected_file_names(webdriver_port, session_id, css_selector):
    return execute_script(
        webdriver_port,
        session_id,
        f"return Array.from(document.querySelector({json.dumps(css_selector)}).files, file => [file.name, file.size]);",
    )


def run_file_upload_tests(webdriver_port, session_id, temp_dir):
    single_path = os.path.join(temp_dir, "single.txt")
    with open(single_path, "w") as file:
        file.write("single file contents")

    first_path = os.path.join(temp_dir, "first.txt")
    with open(first_path, "w") as file:
        file.write("first")

    second_path = os.path.join(temp_dir, "second.txt")
    with open(second_path, "w") as file:
        file.write("second contents")

    # Selecting a single file populates the input's file list with its name and contents. The file is
    # opened by the UI process on behalf of the (potentially sandboxed) WebContent process.
    file_element = find_element(webdriver_port, session_id, "#file")
    expect_send_keys_success(webdriver_port, session_id, file_element, single_path, "single file selection")
    files = selected_file_names(webdriver_port, session_id, "#file")
    if files != [["single.txt", len("single file contents")]]:
        raise AssertionError(f"Expected single selected file, got {files}")

    # A file input without `multiple` only accepts a single path.
    status, payload, response_body = element_send_keys(
        webdriver_port, session_id, file_element, f"{first_path}\n{second_path}"
    )
    value = payload.get("value")
    if status < 400 or not isinstance(value, dict) or value.get("error") != "invalid argument":
        raise AssertionError(f"Expected multi-path selection on single input to fail, got {response_body}")

    # A nonexistent path is rejected with invalid argument.
    missing_path = os.path.join(temp_dir, "does-not-exist.txt")
    expect_send_keys_error(
        webdriver_port, session_id, file_element, missing_path, "invalid argument", "nonexistent file selection"
    )
    files = selected_file_names(webdriver_port, session_id, "#file")
    if files != [["single.txt", len("single file contents")]]:
        raise AssertionError(f"Expected selection to be unchanged after failed selection, got {files}")

    # A `multiple` file input accepts newline-separated paths and appends them in order.
    multiple_element = find_element(webdriver_port, session_id, "#file-multiple")
    expect_send_keys_success(
        webdriver_port, session_id, multiple_element, f"{first_path}\n{second_path}", "multiple file selection"
    )
    files = selected_file_names(webdriver_port, session_id, "#file-multiple")
    if files != [["first.txt", len("first")], ["second.txt", len("second contents")]]:
        raise AssertionError(f"Expected two selected files, got {files}")


def run_non_typeable_form_control_tests(webdriver_port, session_id):
    # A mutable non-typeable form control has its value set directly.
    color_element = find_element(webdriver_port, session_id, "#color")
    expect_send_keys_success(webdriver_port, session_id, color_element, "#ff0000", "color value")
    color_value = execute_script(webdriver_port, session_id, "return document.querySelector('#color').value;")
    if color_value != "#ff0000":
        raise AssertionError(f"Expected color input value to be #ff0000, got {color_value}")


def run_test(webdriver_binary):
    page_server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), TestPageHandler)
    page_server_thread = threading.Thread(target=page_server.serve_forever, daemon=True)
    page_server_thread.start()

    webdriver_port = unused_port()

    webdriver_stdout = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    webdriver_stderr = tempfile.TemporaryFile(mode="w+", encoding="utf-8")

    webdriver = subprocess.Popen(
        [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(webdriver_port)],
        stdout=webdriver_stdout,
        stderr=webdriver_stderr,
        text=True,
        env=os.environ.copy(),
    )

    session_id = None
    failed = False
    try:
        wait_for_port(webdriver_port)

        session_id = create_session(webdriver_port)
        url = f"http://localhost:{page_server.server_port}/send-keys"
        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url})

        with tempfile.TemporaryDirectory() as temp_dir:
            run_file_upload_tests(webdriver_port, session_id, temp_dir)

        run_non_typeable_form_control_tests(webdriver_port, session_id)
    except Exception:
        failed = True
        raise
    finally:
        if session_id is not None:
            try:
                request(webdriver_port, "DELETE", f"/session/{session_id}")
            except Exception:
                pass

        webdriver.terminate()
        try:
            webdriver.wait(timeout=5)
        except subprocess.TimeoutExpired:
            webdriver.kill()
            webdriver.wait()

        webdriver_stdout.seek(0)
        webdriver_stderr.seek(0)
        stdout = webdriver_stdout.read()
        stderr = webdriver_stderr.read()
        webdriver_stdout.close()
        webdriver_stderr.close()

        page_server.shutdown()
        page_server.server_close()

        if failed or webdriver.returncode not in (0, -15):
            print(stdout, file=sys.stdout)
            print(stderr, file=sys.stderr)
        if webdriver.returncode not in (0, -15):
            raise RuntimeError(f"WebDriver exited with status {webdriver.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()

    run_test(args.webdriver_binary)


if __name__ == "__main__":
    main()
