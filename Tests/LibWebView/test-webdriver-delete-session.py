#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import concurrent.futures
import http.client
import json
import socket
import subprocess
import time

EVENT_TIMEOUT_SECONDS = 30
WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 60


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


def request(webdriver_port, method, path, body=None):
    encoded_body = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"} if body is not None else {}
    connection = http.client.HTTPConnection("127.0.0.1", webdriver_port, timeout=WEBDRIVER_REQUEST_TIMEOUT_SECONDS)
    try:
        connection.request(method, path, encoded_body, headers)
        response = connection.getresponse()
        response_body = response.read().decode()
    finally:
        connection.close()

    payload = json.loads(response_body) if response_body else {}
    return response.status, payload, response_body


def create_session(webdriver_port):
    status, payload, response_body = request(
        webdriver_port,
        "POST",
        "/session",
        {"capabilities": {"alwaysMatch": {"ladybird:headless": True}}},
    )
    value = payload.get("value")
    session_id = value.get("sessionId") if isinstance(value, dict) else None
    if status != 200 or not session_id:
        raise AssertionError(f"New Session failed with HTTP {status}: {response_body}")
    return session_id


def run_test(webdriver_binary):
    webdriver_port = unused_port()
    webdriver = subprocess.Popen([webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(webdriver_port)])
    try:
        wait_for_port(webdriver_port)
        session_id = create_session(webdriver_port)

        expected_responses = (
            (200, {"value": None}),
            (
                404,
                {
                    "value": {
                        "error": "invalid session id",
                        "message": "Invalid session id",
                        "stacktrace": "",
                    }
                },
            ),
        )
        for attempt, (expected_status, expected_payload) in enumerate(expected_responses, start=1):
            status, payload, response_body = request(webdriver_port, "DELETE", f"/session/{session_id}")
            if status != expected_status or payload != expected_payload:
                raise AssertionError(f"Delete Session attempt {attempt} failed with HTTP {status}: {response_body}")

        session_id = create_session(webdriver_port)
        with concurrent.futures.ThreadPoolExecutor(max_workers=3) as executor:
            pending_script = executor.submit(
                request,
                webdriver_port,
                "POST",
                f"/session/{session_id}/execute/async",
                {
                    "script": "setTimeout(arguments[0], 2000);",
                    "args": [],
                },
            )
            time.sleep(0.25)
            pending_deletes = [
                executor.submit(request, webdriver_port, "DELETE", f"/session/{session_id}") for _ in range(2)
            ]

            status, payload, response_body = pending_script.result()
            if status != 200 or payload != {"value": None}:
                raise AssertionError(f"Execute Async Script failed with HTTP {status}: {response_body}")

            delete_responses = [pending_delete.result() for pending_delete in pending_deletes]

        if sorted(status for status, _, _ in delete_responses) != [200, 404]:
            raise AssertionError(f"Queued Delete Session requests returned unexpected responses: {delete_responses}")

        for status, payload, response_body in delete_responses:
            expected_payload = dict(expected_responses)[status]
            if payload != expected_payload:
                raise AssertionError(f"Queued Delete Session failed with HTTP {status}: {response_body}")
    finally:
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
