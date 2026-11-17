#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.client
import importlib
import subprocess

WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 30

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")


def request(port, method, path, headers, body=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=WEBDRIVER_REQUEST_TIMEOUT_SECONDS)
    try:
        connection.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
        for name, value in headers.items():
            connection.putheader(name, value)
        if body is not None:
            connection.putheader("Content-Length", str(len(body)))
        connection.endheaders(body)
        response = connection.getresponse()
        return response.status, response.read().decode()
    finally:
        connection.close()


def expect(port, method, path, headers, expected_status, body=None):
    status, response_body = request(port, method, path, headers, body)
    if status != expected_status:
        raise AssertionError(
            f"{method} {path} with {headers} returned HTTP {status}, expected {expected_status}: {response_body}"
        )


def run_test(webdriver_binary):
    webdriver, port = webdriver_helpers.launch_webdriver(webdriver_binary, "--headless")
    try:
        # A WebDriver client names the listener by IP address or as localhost.
        for host in (f"127.0.0.1:{port}", f"localhost:{port}", f"LOCALHOST:{port}", f"[::1]:{port}", "127.0.0.1"):
            expect(port, "GET", "/status", {"Host": host}, 200)

        # A page using DNS rebinding names its own domain.
        for host in (f"attacker.example:{port}", "attacker.example", "[::1", "[::1]x", ""):
            expect(port, "GET", "/status", {"Host": host}, 403)
        expect(port, "GET", "/status", {"Accept": "application/json"}, 403)

        # A page's request carries an Origin header.
        host = {"Host": f"127.0.0.1:{port}"}
        expect(port, "GET", "/status", {**host, "Origin": "https://attacker.example"}, 403)
        expect(port, "GET", "/status", {**host, "Origin": "null"}, 403)

        # A page can send these without a CORS preflight. No browser must launch for them.
        session_body = b'{"capabilities":{}}'
        for content_type in (
            "text/plain",
            "text/plain;charset=UTF-8",
            "application/x-www-form-urlencoded",
            "multipart/form-data; boundary=x",
        ):
            expect(port, "POST", "/session", {**host, "Content-Type": content_type}, 415, session_body)
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
