#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.client
import json
import socket
import subprocess
import time

EVENT_TIMEOUT_SECONDS = 30
WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 30


def unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port):
    deadline = time.monotonic() + EVENT_TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Timed out waiting for port {port}")


def request(connection, method, path, body=None):
    encoded_body = json.dumps(body).encode() if body is not None else None
    headers = {"Connection": "keep-alive"}
    if body is not None:
        headers["Content-Type"] = "application/json"
    connection.request(method, path, encoded_body, headers)
    response = connection.getresponse()
    response_body = response.read().decode()

    payload = json.loads(response_body) if response_body else {}
    return response.status, payload, response_body


def run_test(webdriver_binary):
    webdriver_port = unused_port()
    webdriver = subprocess.Popen([webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(webdriver_port)])
    connection = None
    try:
        wait_for_port(webdriver_port)

        # Every request in this test is sent over the same keep-alive connection, matching how the
        # WPT wdspec harness drives WebDriver. An error response must leave the connection usable.
        connection = http.client.HTTPConnection("127.0.0.1", webdriver_port, timeout=WEBDRIVER_REQUEST_TIMEOUT_SECONDS)

        status, payload, response_body = request(
            connection,
            "POST",
            "/session",
            {"capabilities": {"alwaysMatch": {"ladybird:headless": True}}},
        )
        value = payload.get("value")
        session_id = value.get("sessionId") if isinstance(value, dict) else None
        if status != 200 or not session_id:
            raise AssertionError(f"New Session failed with HTTP {status}: {response_body}")

        status, payload, response_body = request(
            connection, "POST", f"/session/{session_id}/window", {"handle": "does-not-exist"}
        )
        if status != 404 or payload.get("value", {}).get("error") != "no such window":
            raise AssertionError(f"Switch To Window returned an unexpected response: HTTP {status}: {response_body}")

        status, payload, response_body = request(connection, "GET", f"/session/{session_id}/title")
        if status != 200:
            raise AssertionError(f"Get Title after error failed with HTTP {status}: {response_body}")

        status, payload, response_body = request(connection, "GET", f"/session/{session_id}/unknown-command")
        if status != 404 or payload.get("value", {}).get("error") != "unknown command":
            raise AssertionError(f"Unknown command returned an unexpected response: HTTP {status}: {response_body}")

        status, payload, response_body = request(connection, "GET", f"/session/{session_id}/title")
        if status != 200:
            raise AssertionError(f"Get Title after unknown command failed with HTTP {status}: {response_body}")

        status, payload, response_body = request(connection, "DELETE", f"/session/{session_id}")
        if status != 200:
            raise AssertionError(f"Delete Session failed with HTTP {status}: {response_body}")
    finally:
        if connection is not None:
            connection.close()
        webdriver.terminate()
        try:
            webdriver.wait(timeout=5)
        except subprocess.TimeoutExpired:
            webdriver.kill()
            webdriver.wait()

    if webdriver.returncode not in (0, -15):
        raise RuntimeError(f"WebDriver exited with status {webdriver.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()

    run_test(args.webdriver_binary)


if __name__ == "__main__":
    main()
