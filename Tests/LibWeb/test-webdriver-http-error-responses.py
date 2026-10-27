#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import gzip
import http.client
import http.server
import json
import socket
import subprocess
import tempfile
import threading
import time

from typing import cast

EVENT_TIMEOUT_SECONDS = 30
WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 60


class TestPageServer(http.server.ThreadingHTTPServer):
    def __init__(self):
        super().__init__(("127.0.0.1", 0), TestPageHandler)
        self.recovery_requests = {}
        self.recovery_lock = threading.Lock()


class TestPageHandler(http.server.BaseHTTPRequestHandler):
    def respond(self, status, body=b"", headers=None, delay=0.0):
        self.send_response(status)
        headers = {
            "Content-Type": "text/html",
            "Content-Length": str(len(body)),
            "Cache-Control": "no-store",
            **(headers or {}),
        }
        for name, value in headers.items():
            if value is not None:
                self.send_header(name, value)
        self.end_headers()
        if delay:
            self.wfile.flush()
            time.sleep(delay)
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/recover/"):
            server = cast(TestPageServer, self.server)
            with server.recovery_lock:
                request_count = server.recovery_requests.get(self.path, 0) + 1
                server.recovery_requests[self.path] = request_count
            status = int(self.path.split("/")[2]) if request_count == 1 else 200
            body = b"" if request_count == 1 else b"<!doctype html><p>Recovered page</p>"
            self.respond(status, body)
            return

        if self.path.startswith("/http-response/"):
            _, _, status, body_kind = self.path.split("/")
            body = {
                "empty": b"",
                "no-length": b"",
                "compressed-empty": gzip.compress(b""),
                "plain-empty": b"",
                "plain-text": b"Server error page",
                "one-byte": b"x",
                "custom": b"<!doctype html><p>Server error page</p>",
                "delayed": b"<!doctype html><p>Delayed server error page</p>",
            }[body_kind]
            headers = {
                "Content-Type": "text/plain" if body_kind.startswith("plain-") else "text/html",
                "Content-Length": None if body_kind == "no-length" else str(len(body)),
            }
            if body_kind == "compressed-empty":
                headers["Content-Encoding"] = "gzip"
            self.respond(int(status), body, headers, delay=0.1 if body_kind == "delayed" else 0)
            return

        if self.path.startswith("/redirect/"):
            server = cast(http.server.HTTPServer, self.server)
            target = self.path.removeprefix("/redirect")
            self.respond(302, headers={"Location": f"http://127.0.0.1:{server.server_port}{target}"})
            return

        if self.path == "/source":
            self.respond(200, b"<!doctype html><p>Source page</p>")
            return

        self.send_error(404)

    def log_message(self, format, *args):
        pass


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
    if response.status != 200:
        raise RuntimeError(f"{method} {path} failed with HTTP {response.status}: {response_body}")
    return payload["value"]


def check_responses(webdriver_port, session_id, page_port):
    base_url = f"http://localhost:{page_port}"
    redirect_origin = f"http://127.0.0.1:{page_port}"
    source_url = f"{base_url}/source"

    def command(path, body=None, method="POST"):
        return request(webdriver_port, method, f"/session/{session_id}/{path}", body)

    def script(source, asynchronous=False):
        return command("execute/async" if asynchronous else "execute/sync", {"script": source, "args": []})

    def navigate(url):
        command("url", {"url": url})

    def expect(actual, expected):
        if actual != expected:
            raise AssertionError(f"Expected {expected!r}, got {actual!r}")

    def expect_document(url, text, origin=base_url):
        expect(script("return [document.URL, window.origin, document.body.innerText];"), [url, origin, text])

    def expect_error(url, status):
        # NB: The failed URL is in the error page's details, which are collapsed, so it isn't part of innerText.
        state = script("return [document.URL, window.origin, document.body.textContent];")
        if state[:2] != ["about:error", "null"] or f"HTTP error: {status}" not in state[2] or url not in state[2]:
            raise AssertionError(f"Expected an HTTP {status} error document for {url}, got {state!r}")

    def back_to_source():
        command("back", {})
        expect_document(source_url, "Source page")

    # Empty error responses should show the internal HTTP error page.
    for status, body_kind in [
        (400, "empty"),
        (403, "empty"),
        (404, "empty"),
        (500, "empty"),
        (503, "empty"),
        (599, "empty"),
        (404, "no-length"),
        (500, "compressed-empty"),
        (404, "plain-empty"),
        (500, "plain-empty"),
    ]:
        url = f"{base_url}/http-response/{status}/{body_kind}"
        navigate(url)
        expect_error(url, status)

    # Non-empty/non-error responses should show the actual server response.
    for status, body_kind, expected_text in [
        (200, "empty", ""),
        (200, "plain-empty", ""),
        (404, "one-byte", "x"),
        (500, "custom", "Server error page"),
        (500, "plain-text", "Server error page"),
        (503, "delayed", "Delayed server error page"),
    ]:
        url = f"{base_url}/http-response/{status}/{body_kind}"
        navigate(url)
        expect_document(url, expected_text)
        content_type = "text/plain" if body_kind.startswith("plain-") else "text/html"
        expect(script("return document.contentType;"), content_type)

    # Fetch and child navigations still expose the server response.
    navigate(source_url)
    state = script(
        """
        const done = arguments[0];
        (async () => {
            const response = await fetch('/http-response/404/empty');
            const body = await response.text();
            const iframe = document.createElement('iframe');
            const loaded = new Promise(resolve => iframe.onload = resolve);
            iframe.src = '/http-response/500/empty';
            document.body.appendChild(iframe);
            await loaded;
            done([response.status, body, iframe.contentDocument.URL, iframe.contentDocument.body.innerText]);
        })();
        """,
        asynchronous=True,
    )
    expect(state, [404, "", f"{base_url}/http-response/500/empty", ""])
    navigate(source_url)

    # The error page names the redirect target, and back navigation restores the previous document.
    navigate(f"{base_url}/redirect/http-response/404/empty")
    expect_error(f"{redirect_origin}/http-response/404/empty", 404)
    back_to_source()

    # Reload and forward navigation retry the failed URL and recover within the same history entry.
    for status, recovery, redirected in [
        (404, "refresh", False),
        (404, "forward", False),
        (503, "refresh", False),
        (503, "forward", False),
        (404, "refresh", True),
        (404, "forward", True),
    ]:
        path = f"/recover/{status}/{'redirect-' if redirected else ''}{recovery}"
        final_origin = redirect_origin if redirected else base_url
        final_url = f"{final_origin}{path}"
        navigate(source_url)
        navigate(f"{base_url}{'/redirect' if redirected else ''}{path}")
        expect_error(final_url, status)
        history_length = script("return history.length;")
        if recovery == "forward":
            back_to_source()
        command(recovery, {})
        expect_document(final_url, "Recovered page", final_origin)
        expect(script("return history.length;"), history_length)
        back_to_source()


def run_test(webdriver_binary):
    page_server = TestPageServer()
    threading.Thread(target=page_server.serve_forever, daemon=True).start()
    webdriver_port = unused_port()
    with tempfile.TemporaryDirectory() as profile, tempfile.TemporaryFile(mode="w+") as log:
        webdriver = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(webdriver_port), "--profile-path", profile],
            stdout=log,
            stderr=log,
        )
        session_id = None
        try:
            wait_for_port(webdriver_port)
            session = request(
                webdriver_port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": True}}}
            )
            session_id = session["sessionId"]
            request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
            check_responses(webdriver_port, session_id, page_server.server_port)
        except Exception:
            log.seek(0)
            print(log.read())
            raise
        finally:
            try:
                if session_id is not None:
                    request(webdriver_port, "DELETE", f"/session/{session_id}")
            finally:
                webdriver.terminate()
                try:
                    webdriver.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    webdriver.kill()
                    webdriver.wait()
                page_server.shutdown()
                page_server.server_close()

        if webdriver.returncode not in (0, -15):
            raise RuntimeError(f"WebDriver exited with status {webdriver.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)


if __name__ == "__main__":
    main()
