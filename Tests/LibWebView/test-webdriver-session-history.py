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

from typing import cast

BLOCKED_RESPONSE_TIMEOUT_SECONDS = 30
EVENT_TIMEOUT_SECONDS = 30
WEBDRIVER_REQUEST_TIMEOUT_SECONDS = 60


class TestPageServer(http.server.ThreadingHTTPServer):
    def __init__(self, server_address, request_handler_class):
        super().__init__(server_address, request_handler_class)
        self.blocked_frame_b_requested = threading.Event()
        self.frame_b_blocked_document_ran = threading.Event()
        self.release_blocked_frame_b = threading.Event()
        self.blocked_reload_requested = threading.Event()
        self.reload_recovery_requested = threading.Event()
        self.release_blocked_reload = threading.Event()
        self.reload_blocked_request_lock = threading.Lock()
        self.reload_blocked_request_count = 0
        self.blocked_process_swap_back_requested = threading.Event()
        self.process_swap_back_recovery_requested = threading.Event()
        self.process_swap_back_document_ran = threading.Event()
        self.release_blocked_process_swap_back = threading.Event()
        self.process_swap_back_blocked_request_lock = threading.Lock()
        self.process_swap_back_blocked_request_count = 0
        self.block_forward_load = False
        self.blocked_forward_requested = threading.Event()
        self.release_blocked_forward = threading.Event()
        self.blocked_cross_site_post_requested = threading.Event()
        self.release_blocked_cross_site_post = threading.Event()
        self.blocked_same_site_post_requested = threading.Event()
        self.release_blocked_same_site_post = threading.Event()
        self.blocked_same_url_post_requested = threading.Event()
        self.release_blocked_same_url_post = threading.Event()
        self.state_replace_load_document_ran = threading.Event()
        self.a_document_ran = threading.Event()
        self.b_document_ran = threading.Event()
        self.c_document_ran = threading.Event()
        self.d_document_ran = threading.Event()
        self.post_result_document_ran = threading.Event()
        self.cross_site_post_result_document_ran = threading.Event()
        self.same_site_post_result_document_ran = threading.Event()
        self.same_url_post_result_document_ran = threading.Event()
        self.reload_blocked_document_ran = threading.Event()

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], BrokenPipeError):
            return
        super().handle_error(request, client_address)


class TestPageHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        server = cast(TestPageServer, self.server)
        server_port = server.server_port

        if self.path == "/a":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>A</title>
<script>fetch('/document-ran?a');</script>
<a id="go" href="http://127.0.0.1:{server_port}/b">B</a>
<a id="redirect" href="http://localhost:{server_port}/redirect-to-b">Redirect to B</a>
<p>A</p>""".encode()
            )
            return

        if self.path == "/document-ran?a":
            server.a_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/redirect-to-b":
            self.send_response(302)
            self.send_header("Location", f"http://127.0.0.1:{server_port}/b")
            self.end_headers()
            return

        if self.path.startswith("/nested"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Nested</title>
<iframe id="frame" src="http://localhost:{server_port}/frame-a"></iframe>
<p>Nested</p>""".encode()
            )
            return

        if self.path.startswith("/state"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            document_ready_script = ""
            if self.path == "/state?replace-load":
                document_ready_script = "<script>fetch('/document-ran?state-replace-load');</script>"
            self.wfile.write(
                f"""<!doctype html>
<title>State</title>
{document_ready_script}
<p>State</p>""".encode()
            )
            return

        if self.path.startswith("/scroll"):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Scroll</title>
<style>
body {
    margin: 0;
    min-height: 4000px;
}
</style>
<p>Scroll</p>""".encode()
            )
            return

        if self.path == "/reload-blocked":
            should_block = False
            with server.reload_blocked_request_lock:
                server.reload_blocked_request_count += 1
                if server.reload_blocked_request_count > 2:
                    server.reload_recovery_requested.set()
                should_block = server.reload_blocked_request_count > 1

            if should_block:
                server.blocked_reload_requested.set()
                if not server.release_blocked_reload.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                    self.send_response(500)
                    self.send_header("Content-Type", "text/plain")
                    self.end_headers()
                    self.wfile.write(b"timed out waiting to unblock reload")
                    return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Reload Blocked</title>
<script>fetch('/document-ran?reload-blocked');</script>
<p>Reload Blocked</p>""".encode()
            )
            return

        if self.path == "/process-swap-back-blocked":
            should_block = False
            with server.process_swap_back_blocked_request_lock:
                server.process_swap_back_blocked_request_count += 1
                if server.process_swap_back_blocked_request_count > 2:
                    server.process_swap_back_recovery_requested.set()
                should_block = server.process_swap_back_blocked_request_count > 1

            if should_block:
                server.blocked_process_swap_back_requested.set()
                if not server.release_blocked_process_swap_back.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                    self.send_response(500)
                    self.send_header("Content-Type", "text/plain")
                    self.end_headers()
                    self.wfile.write(b"timed out waiting to unblock process-swap back")
                    return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Process Swap Back Blocked</title>
<script>fetch('/document-ran?process-swap-back-blocked');</script>
<a id="go" href="http://127.0.0.1:{server_port}/b">B</a>
<p>Process Swap Back Blocked</p>""".encode()
            )
            return

        if self.path == "/forward-blocked":
            if server.block_forward_load:
                server.blocked_forward_requested.set()
                if not server.release_blocked_forward.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                    self.send_response(500)
                    self.send_header("Content-Type", "text/plain")
                    self.end_headers()
                    self.wfile.write(b"timed out waiting to unblock forward")
                    return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Forward Blocked</title>
<p>Forward Blocked</p>""".encode()
            )
            return

        if self.path == "/frame-a":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Frame A</title>
<p>Frame A</p>""".encode()
            )
            return

        if self.path == "/frame-b":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Frame B</title>
<p>Frame B</p>""".encode()
            )
            return

        if self.path == "/frame-b-blocked":
            server.blocked_frame_b_requested.set()
            if not server.release_blocked_frame_b.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"timed out waiting to unblock frame")
                return
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Frame B Blocked</title>
<script>addEventListener('load', () => fetch('/document-ran?frame-b-blocked'));</script>
<p>Frame B Blocked</p>""".encode()
            )
            return

        if self.path == "/b":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>B</title>
<script>fetch('/document-ran?b');</script>
<a id="go" href="http://127.0.0.1:{server_port}/c">C</a>
<a id="branch" href="http://localhost:{server_port}/d">D</a>
<p>B</p>""".encode()
            )
            return

        if self.path == "/document-ran?b":
            server.b_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/c":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>C</title>
<script>fetch('/document-ran?c');</script>
<a id="go" href="http://localhost:{server_port}/a">A</a>
<p>C</p>""".encode()
            )
            return

        if self.path == "/document-ran?state-replace-load":
            server.state_replace_load_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?c":
            server.c_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?process-swap-back-blocked":
            server.process_swap_back_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?frame-b-blocked":
            server.frame_b_blocked_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?post-result":
            server.post_result_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?cross-site-post-result":
            server.cross_site_post_result_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?same-site-post-result":
            server.same_site_post_result_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?same-url-post-result":
            server.same_url_post_result_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/document-ran?reload-blocked":
            server.reload_blocked_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/d":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>D</title>
<script>fetch('/document-ran?d');</script>
<script>
window.initialHistoryLength = history.length;
window.initialNavigationEntryCount = navigation.entries().length;
window.initialNavigationCurrentIndex = navigation.currentEntry.index;
</script>
<p>D</p>""".encode()
            )
            return

        if self.path == "/document-ran?d":
            server.d_document_ran.set()
            self.send_response(204)
            self.end_headers()
            return

        if self.path == "/post-form":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Post Form</title>
<form id="post-form" method="post" action="/post-result">
<input name="name" value="ladybird">
<button id="submit">Submit</button>
</form>
<p>Post Form</p>""".encode()
            )
            return

        if self.path == "/post-blocked-form":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Post Blocked Form</title>
<form id="post-form" method="post" action="/post-result-blocked-same-site">
<input name="name" value="ladybird">
<button id="submit">Submit</button>
</form>
<p>Post Blocked Form</p>""".encode()
            )
            return

        if self.path == "/post-same-url-blocked":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Post Same URL Blocked Form</title>
<form id="post-form" method="post" action="/post-same-url-blocked">
<input name="name" value="ladybird">
<button id="submit">Submit</button>
</form>
<p>Post Same URL Blocked Form</p>""".encode()
            )
            return

        if self.path == "/cross-site-post-form":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Cross-site Post Form</title>
<form id="post-form" method="post" action="http://127.0.0.1:{server_port}/post-result">
<input name="name" value="ladybird">
<button id="submit">Submit</button>
</form>
<p>Cross-site Post Form</p>""".encode()
            )
            return

        if self.path == "/cross-site-post-blocked-form":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Cross-site Post Blocked Form</title>
<form id="post-form" method="post" action="http://127.0.0.1:{server_port}/post-result-blocked">
<input name="name" value="ladybird">
<button id="submit">Submit</button>
</form>
<p>Cross-site Post Blocked Form</p>""".encode()
            )
            return

        if self.path == "/post-result":
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                """<!doctype html>
<title>Post Result GET</title>
<p id="result">GET</p>""".encode()
            )
            return

        self.send_response(404)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(b"not found")

    def do_POST(self):
        server = cast(TestPageServer, self.server)

        if self.path == "/post-result-blocked":
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length).decode()
            server.blocked_cross_site_post_requested.set()
            if not server.release_blocked_cross_site_post.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"timed out waiting to unblock cross-site POST")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Post Result Blocked POST</title>
<script>fetch('/document-ran?cross-site-post-result');</script>
<p id="result">POST:{body}</p>""".encode()
            )
            return

        if self.path == "/post-result-blocked-same-site":
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length).decode()
            server.blocked_same_site_post_requested.set()
            if not server.release_blocked_same_site_post.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"timed out waiting to unblock same-site POST")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Post Result Blocked Same-Site POST</title>
<script>fetch('/document-ran?same-site-post-result');</script>
<p id="result">POST:{body}</p>""".encode()
            )
            return

        if self.path == "/post-same-url-blocked":
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length).decode()
            server.blocked_same_url_post_requested.set()
            if not server.release_blocked_same_url_post.wait(timeout=BLOCKED_RESPONSE_TIMEOUT_SECONDS):
                self.send_response(500)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"timed out waiting to unblock same-URL POST")
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Post Same URL Blocked POST</title>
<script>fetch('/document-ran?same-url-post-result');</script>
<p id="result">POST:{body}</p>""".encode()
            )
            return

        if self.path == "/post-result":
            content_length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(content_length).decode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(
                f"""<!doctype html>
<title>Post Result POST</title>
<script>fetch('/document-ran?post-result');</script>
<p id="result">POST:{body}</p>""".encode()
            )
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


def wait_for_event(event, label, timeout=EVENT_TIMEOUT_SECONDS):
    if event.wait(timeout=timeout):
        return
    raise RuntimeError(f"Timed out waiting for {label}")


def count_open_fds(pid):
    proc_fd = f"/proc/{pid}/fd"
    if os.path.isdir(proc_fd):
        return len(os.listdir(proc_fd))
    try:
        result = subprocess.run(["lsof", "-p", str(pid)], capture_output=True, text=True)
    except FileNotFoundError:
        return None
    return max(0, len(result.stdout.splitlines()) - 1)


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


def current_url(webdriver_port, session_id):
    return request(webdriver_port, "GET", f"/session/{session_id}/url")["value"]


def execute_script(webdriver_port, session_id, script):
    return request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/execute/sync",
        {
            "script": script,
            "args": [],
        },
    )["value"]


def wait_for_script_result(webdriver_port, session_id, label, script, predicate, log, timeout=EVENT_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    result = None
    while time.monotonic() < deadline:
        result = execute_script(webdriver_port, session_id, script)
        if predicate(result):
            log.append(f"{label}: {result}")
            return result
        time.sleep(0.05)

    raise AssertionError(f"Timed out waiting for {label}, got {result}\n" + "\n".join(log))


def execute_async_script(webdriver_port, session_id, script):
    return request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/execute/async",
        {
            "script": script,
            "args": [],
        },
    )["value"]


def navigate_from_renderer_using_link(
    webdriver_port,
    session_id,
    link_selector,
    expected_url,
    log,
    document_ran_event,
    document_ran_label,
    expected_scheduled_url=None,
):
    actual_url = execute_script(
        webdriver_port,
        session_id,
        f"""
const link = document.querySelector({json.dumps(link_selector)});
if (!link)
    return null;
return link.href;
""",
    )
    log.append(f"clicked renderer link to {actual_url}")
    expected_scheduled_url = expected_scheduled_url or expected_url
    if actual_url != expected_scheduled_url:
        raise AssertionError(
            f"Expected renderer navigation target to be {expected_scheduled_url}, got {actual_url}\n" + "\n".join(log)
        )
    document_ran_event.clear()
    execute_script(
        webdriver_port,
        session_id,
        f"""
const link = document.querySelector({json.dumps(link_selector)});
link.click();
return null;
""",
    )
    wait_for_event(document_ran_event, document_ran_label)


def crash_current_page(webdriver_port, session_id):
    request(webdriver_port, "POST", f"/session/{session_id}/ladybird/crash-current-page", {})


def crash_current_page_allowing_navigation_timeout(webdriver_port, session_id):
    try:
        status, payload, response_body = request_raw(
            webdriver_port, "POST", f"/session/{session_id}/ladybird/crash-current-page", {}
        )
    except TimeoutError:
        return

    if status < 400:
        return

    value = payload.get("value")
    if isinstance(value, dict) and value.get("error") == "timeout":
        return

    raise RuntimeError(
        f"POST /session/{session_id}/ladybird/crash-current-page failed with HTTP {status}: {response_body}"
    )


def load_url_from_ui(webdriver_port, session_id, url):
    request(webdriver_port, "POST", f"/session/{session_id}/ladybird/load-url-from-ui", {"url": url})


def traverse_history_from_ui(webdriver_port, session_id, delta, wait_for_navigation_completion=True):
    request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/ladybird/traverse-history-from-ui",
        {
            "delta": delta,
            "waitForNavigationCompletion": wait_for_navigation_completion,
        },
    )


def mark_web_content_session_history_stale(webdriver_port, session_id):
    request(webdriver_port, "POST", f"/session/{session_id}/ladybird/mark-web-content-session-history-stale", {})


def refresh(webdriver_port, session_id):
    request(webdriver_port, "POST", f"/session/{session_id}/refresh", {})


def create_session(webdriver_port, enable_test_hooks=True):
    always_match = {
        "ladybird:headless": True,
        "pageLoadStrategy": "normal",
        "unhandledPromptBehavior": {"beforeUnload": "dismiss"},
    }
    if enable_test_hooks:
        always_match["ladybird:enableTestHooks"] = True

    created = request(
        webdriver_port,
        "POST",
        "/session",
        {"capabilities": {"alwaysMatch": always_match}},
    )
    session_id = created.get("value", {}).get("sessionId") or created.get("sessionId")
    if not session_id:
        raise RuntimeError(f"Could not find session id in response: {created}")

    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
    return session_id


def expect_ladybird_test_hooks_require_capability(webdriver_port):
    session_id = create_session(webdriver_port, enable_test_hooks=False)
    try:
        status, payload, response_body = request_raw(
            webdriver_port, "GET", f"/session/{session_id}/ladybird/session-history"
        )
        value = payload.get("value")
        if status != 404 or not isinstance(value, dict) or value.get("error") != "unknown command":
            raise AssertionError(
                f"Expected Ladybird test hook to fail with unknown command, got HTTP {status}: {response_body}"
            )
    finally:
        request(webdriver_port, "DELETE", f"/session/{session_id}")


def browser_history_shortcut_modifier():
    if sys.platform == "darwin":
        return "\ue03d", "Meta"
    return "\ue00a", "Alt"


def perform_browser_history_shortcut(webdriver_port, session_id, direction, log):
    if direction == "left":
        arrow = "\ue012"
    elif direction == "right":
        arrow = "\ue014"
    else:
        raise ValueError(f"Unknown arrow direction {direction}")

    modifier, modifier_name = browser_history_shortcut_modifier()
    request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/actions",
        {
            "actions": [
                {
                    "type": "key",
                    "id": "browser-shortcut-keyboard",
                    "actions": [
                        {"type": "keyDown", "value": modifier},
                        {"type": "keyDown", "value": arrow},
                        {"type": "keyUp", "value": arrow},
                        {"type": "keyUp", "value": modifier},
                    ],
                }
            ]
        },
    )
    log.append(f"performed {modifier_name}+{direction}")


def perform_pointer_click(webdriver_port, session_id, x, y, log):
    request(
        webdriver_port,
        "POST",
        f"/session/{session_id}/actions",
        {
            "actions": [
                {
                    "type": "pointer",
                    "id": "activation-pointer",
                    "parameters": {"pointerType": "mouse"},
                    "actions": [
                        {"type": "pointerMove", "origin": "viewport", "x": x, "y": y},
                        {"type": "pointerDown", "button": 0},
                        {"type": "pointerUp", "button": 0},
                    ],
                }
            ]
        },
    )
    log.append(f"clicked viewport at {x},{y}")


def session_history(webdriver_port, session_id):
    return request(webdriver_port, "GET", f"/session/{session_id}/ladybird/session-history")["value"]


def wait_for_session_history(webdriver_port, session_id, label, predicate, log, timeout=EVENT_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    snapshot = None
    while time.monotonic() < deadline:
        snapshot = session_history(webdriver_port, session_id)
        if predicate(snapshot):
            log.append(f"{label} history: {summarize_history_snapshot(snapshot)}")
            return snapshot
        time.sleep(0.05)

    raise AssertionError(
        f"Timed out waiting for {label} history, got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def history_entry_urls(history):
    return [entry["url"] for entry in history["entries"]]


def history_used_steps(history):
    return history_step_values(history["usedSteps"])


def history_step_values(steps):
    return [used_step["step"] for used_step in steps]


def history_current_step(steps):
    current_steps = [used_step["step"] for used_step in steps if used_step.get("current", False)]
    if len(current_steps) > 1:
        raise AssertionError(f"Expected at most one current step, got {current_steps}")
    if not current_steps:
        return None
    return current_steps[0]


def history_current_entry(history):
    current_entries = [entry for entry in history["entries"] if entry["current"]]
    if len(current_entries) != 1:
        raise AssertionError(f"Expected exactly one current entry, got {current_entries}")
    return current_entries[0]


def comparable_history(history):
    def comparable_entry(entry):
        return {
            "step": entry["step"],
            "url": entry["url"],
            "current": entry.get("current", False),
            "nestedHistories": [
                {
                    "id": nested_history["id"],
                    "entries": [comparable_entry(nested_entry) for nested_entry in nested_history["entries"]],
                }
                for nested_history in entry["nestedHistories"]
            ],
        }

    return {
        "currentUsedStepIndex": history["currentUsedStepIndex"],
        "entries": [comparable_entry(entry) for entry in history["entries"]],
        "usedSteps": history["usedSteps"],
    }


def summarize_history_snapshot(snapshot):
    ui = snapshot["ui"]
    web_content = snapshot["webContent"]
    return {
        "ui": {
            "entries": history_entry_urls(ui),
            "usedSteps": history_used_steps(ui),
            "currentUsedStepIndex": ui["currentUsedStepIndex"],
            "back": ui["backButtonEnabled"],
            "forward": ui["forwardButtonEnabled"],
            "matches": ui["webContentHistoryMatchesUI"],
            "waitingToSeedWebContent": ui["waitingToSeedWebContent"],
            "waitingForWebContentSeedAck": ui["waitingForWebContentSeedAck"],
            "ignoringWebContentUpdatesUntilSeed": ui["ignoringWebContentUpdatesUntilSeed"],
            "reseedAfterCurrentHistoryLoad": ui["reseedAfterCurrentHistoryLoad"],
            "webContentUsesUIStepCoordinates": ui["webContentUsesUIStepCoordinates"],
            "webContentKnownUsedSteps": history_step_values(ui["webContentKnownUsedSteps"]),
            "webContentCurrentStep": ui["webContentCurrentStep"],
            "pendingWebContentHistoryStepAfterFallbackLoad": ui["pendingWebContentHistoryStepAfterFallbackLoad"],
            "pendingSessionHistoryNavigation": ui["pendingSessionHistoryNavigation"],
            "pendingSessionHistoryTraversal": ui["pendingSessionHistoryTraversal"],
            "currentResource": history_current_entry(ui).get("resource"),
        },
        "webContent": {
            "entries": history_entry_urls(web_content),
            "usedSteps": history_used_steps(web_content),
            "currentUsedStepIndex": web_content["currentUsedStepIndex"],
            "currentResource": history_current_entry(web_content).get("resource"),
        },
    }


def expect_ui_session_history(
    webdriver_port,
    session_id,
    label,
    expected_entry_urls,
    expected_used_steps,
    expected_current_used_step_index,
    expected_back_enabled,
    expected_forward_enabled,
    log,
    expect_web_content_matches_ui=None,
    expected_web_content_known_used_steps=None,
    expected_web_content_current_step=None,
    expected_waiting_to_seed_web_content=None,
    expected_waiting_for_web_content_seed_ack=None,
    expected_ignoring_web_content_updates_until_seed=None,
    expected_reseed_after_current_history_load=None,
):
    def ui_history_matches(ui):
        return (
            history_entry_urls(ui) == expected_entry_urls
            and history_used_steps(ui) == expected_used_steps
            and ui["currentUsedStepIndex"] == expected_current_used_step_index
            and ui["backButtonEnabled"] is expected_back_enabled
            and ui["forwardButtonEnabled"] is expected_forward_enabled
            and (
                expect_web_content_matches_ui is None
                or ui["webContentHistoryMatchesUI"] is expect_web_content_matches_ui
            )
            and (
                expected_web_content_known_used_steps is None
                or history_step_values(ui["webContentKnownUsedSteps"]) == expected_web_content_known_used_steps
            )
            and (
                expected_web_content_current_step is None
                or ui["webContentCurrentStep"] == expected_web_content_current_step
            )
            and (
                expected_web_content_current_step is None
                or history_current_step(ui["webContentKnownUsedSteps"]) == expected_web_content_current_step
            )
            and (
                expected_waiting_to_seed_web_content is None
                or ui["waitingToSeedWebContent"] is expected_waiting_to_seed_web_content
            )
            and (
                expected_waiting_for_web_content_seed_ack is None
                or ui["waitingForWebContentSeedAck"] is expected_waiting_for_web_content_seed_ack
            )
            and (
                expected_ignoring_web_content_updates_until_seed is None
                or ui["ignoringWebContentUpdatesUntilSeed"] is expected_ignoring_web_content_updates_until_seed
            )
            and (
                expected_reseed_after_current_history_load is None
                or ui["reseedAfterCurrentHistoryLoad"] is expected_reseed_after_current_history_load
            )
        )

    def web_content_matches_ui(snapshot):
        ui = snapshot["ui"]
        web_content = snapshot["webContent"]
        return not (
            ui["waitingToSeedWebContent"]
            or ui["waitingForWebContentSeedAck"]
            or ui["ignoringWebContentUpdatesUntilSeed"]
            or ui["reseedAfterCurrentHistoryLoad"]
            or ui["pendingWebContentHistoryStepAfterFallbackLoad"] is not None
            or ui["pendingSessionHistoryNavigation"] is not None
            or ui["pendingSessionHistoryTraversal"] is not None
            or comparable_history(ui) != comparable_history(web_content)
        )

    def raise_ui_mismatch(snapshot):
        raise AssertionError(
            f"Expected {label} UI history to be entries={expected_entry_urls}, usedSteps={expected_used_steps}, "
            f"currentUsedStepIndex={expected_current_used_step_index}, back={expected_back_enabled}, "
            f"forward={expected_forward_enabled}, webContentMatchesUI={expect_web_content_matches_ui}, "
            f"webContentKnownUsedSteps={expected_web_content_known_used_steps}, "
            f"webContentCurrentStep={expected_web_content_current_step}, "
            f"waitingToSeedWebContent={expected_waiting_to_seed_web_content}, "
            f"waitingForWebContentSeedAck={expected_waiting_for_web_content_seed_ack}, "
            f"ignoringWebContentUpdatesUntilSeed={expected_ignoring_web_content_updates_until_seed}; "
            f"reseedAfterCurrentHistoryLoad={expected_reseed_after_current_history_load}; "
            f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
        )

    if expect_web_content_matches_ui:
        deadline = time.monotonic() + EVENT_TIMEOUT_SECONDS
        while True:
            snapshot = session_history(webdriver_port, session_id)
            if ui_history_matches(snapshot["ui"]) and web_content_matches_ui(snapshot):
                break
            if time.monotonic() >= deadline:
                if not ui_history_matches(snapshot["ui"]):
                    raise_ui_mismatch(snapshot)
                raise AssertionError(
                    f"Expected {label} WebContent history to match UI, got {summarize_history_snapshot(snapshot)}\n"
                    + "\n".join(log)
                )
            time.sleep(0.05)

        log.append(f"{label} history: {summarize_history_snapshot(snapshot)}")
        return snapshot

    snapshot = session_history(webdriver_port, session_id)
    if not ui_history_matches(snapshot["ui"]):
        raise_ui_mismatch(snapshot)

    log.append(f"{label} history: {summarize_history_snapshot(snapshot)}")
    return snapshot


def wait_for_ui_session_history(
    webdriver_port,
    session_id,
    label,
    expected_entry_urls,
    expected_used_steps,
    expected_current_used_step_index,
    expected_back_enabled,
    expected_forward_enabled,
    log,
    expected_web_content_known_used_steps,
    expected_web_content_current_step,
):
    def matches_expected_history(snapshot):
        ui = snapshot["ui"]
        web_content = snapshot["webContent"]
        return (
            history_entry_urls(ui) == expected_entry_urls
            and history_used_steps(ui) == expected_used_steps
            and ui["currentUsedStepIndex"] == expected_current_used_step_index
            and ui["backButtonEnabled"] is expected_back_enabled
            and ui["forwardButtonEnabled"] is expected_forward_enabled
            and ui["webContentHistoryMatchesUI"]
            and history_step_values(ui["webContentKnownUsedSteps"]) == expected_web_content_known_used_steps
            and ui["webContentCurrentStep"] == expected_web_content_current_step
            and history_current_step(ui["webContentKnownUsedSteps"]) == expected_web_content_current_step
            and not ui["waitingToSeedWebContent"]
            and not ui["waitingForWebContentSeedAck"]
            and not ui["ignoringWebContentUpdatesUntilSeed"]
            and not ui["reseedAfterCurrentHistoryLoad"]
            and ui["pendingWebContentHistoryStepAfterFallbackLoad"] is None
            and ui["pendingSessionHistoryNavigation"] is None
            and ui["pendingSessionHistoryTraversal"] is None
            and comparable_history(ui) == comparable_history(web_content)
        )

    return wait_for_session_history(webdriver_port, session_id, label, matches_expected_history, log)


def expect_beforeunload_cancels_webdriver_navigation(
    webdriver_port,
    session_id,
    url,
    label,
    expected_url,
    previous_beforeunload_count,
    expected_history_snapshot,
    log,
):
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 1000})
    request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url})
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
    state = execute_script(
        webdriver_port,
        session_id,
        "return [location.href, window.beforeUnloadCount];",
    )
    if state[0] != expected_url or state[1] <= previous_beforeunload_count:
        raise AssertionError(f"Expected beforeunload to cancel {label}, got {state}\n" + "\n".join(log))

    ui_history = expected_history_snapshot["ui"]
    expect_ui_session_history(
        webdriver_port,
        session_id,
        f"after blocked {label}",
        history_entry_urls(ui_history),
        history_used_steps(ui_history),
        ui_history["currentUsedStepIndex"],
        ui_history["backButtonEnabled"],
        ui_history["forwardButtonEnabled"],
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=history_step_values(ui_history["webContentKnownUsedSteps"]),
        expected_web_content_current_step=ui_history["webContentCurrentStep"],
    )
    return state


def expect_javascript_noop_ui_load_does_not_change_history(
    webdriver_port,
    session_id,
    expected_url,
    expected_history_snapshot,
    log,
):
    load_url_from_ui(webdriver_port, session_id, "javascript:window.uiJavascriptNoopRan=true;void(0)")
    state = execute_script(
        webdriver_port,
        session_id,
        "return [location.href, window.uiJavascriptNoopRan === true];",
    )
    if state != [expected_url, True]:
        raise AssertionError(
            f"Expected javascript:void(0) UI load to stay on {expected_url}, got {state}\n" + "\n".join(log)
        )

    ui_history = expected_history_snapshot["ui"]
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after javascript:void(0) UI load",
        history_entry_urls(ui_history),
        history_used_steps(ui_history),
        ui_history["currentUsedStepIndex"],
        ui_history["backButtonEnabled"],
        ui_history["forwardButtonEnabled"],
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=history_step_values(ui_history["webContentKnownUsedSteps"]),
        expected_web_content_current_step=ui_history["webContentCurrentStep"],
    )


def expect_javascript_noop_webdriver_navigation_does_not_change_history(
    webdriver_port,
    session_id,
    expected_url,
    expected_history_snapshot,
    log,
):
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 1000})
    request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": "javascript:void(0)"})
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
    state = execute_script(webdriver_port, session_id, "return location.href;")
    if state != expected_url:
        raise AssertionError(
            f"Expected javascript:void(0) WebDriver navigation to stay on {expected_url}, got {state}\n"
            + "\n".join(log)
        )

    ui_history = expected_history_snapshot["ui"]
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after javascript:void(0) WebDriver navigation",
        history_entry_urls(ui_history),
        history_used_steps(ui_history),
        ui_history["currentUsedStepIndex"],
        ui_history["backButtonEnabled"],
        ui_history["forwardButtonEnabled"],
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=history_step_values(ui_history["webContentKnownUsedSteps"]),
        expected_web_content_current_step=ui_history["webContentCurrentStep"],
    )


def expect_beforeunload_cancels_refresh(
    webdriver_port,
    session_id,
    expected_url,
    previous_beforeunload_count,
    log,
):
    expect_current_ui_entry_reload_pending(webdriver_port, session_id, "before blocked refresh from /b", False, log)
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 1000})
    try:
        refresh(webdriver_port, session_id)
    finally:
        request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})

    def refresh_was_canceled_by_beforeunload(result):
        return (
            isinstance(result, list)
            and len(result) == 2
            and result[0] == expected_url
            and isinstance(result[1], int)
            and result[1] > previous_beforeunload_count
        )

    state = wait_for_script_result(
        webdriver_port,
        session_id,
        "beforeunload-canceled refresh from /b",
        "return [location.href, window.beforeUnloadCount];",
        refresh_was_canceled_by_beforeunload,
        log,
    )
    expect_current_ui_entry_reload_pending(webdriver_port, session_id, "after blocked refresh from /b", False, log)
    return state


def expect_current_top_level_history_url(webdriver_port, session_id, label, expected_url, log):
    snapshot = session_history(webdriver_port, session_id)
    current_entry = history_current_entry(snapshot["ui"])
    if current_entry["url"] == expected_url:
        log.append(f"{label} current history URL: {expected_url}")
        return snapshot

    raise AssertionError(
        f"Expected {label} current history URL to be {expected_url}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_navigation_buttons(webdriver_port, session_id, label, expected_back_enabled, expected_forward_enabled, log):
    snapshot = session_history(webdriver_port, session_id)
    ui = snapshot["ui"]
    if ui["backButtonEnabled"] is expected_back_enabled and ui["forwardButtonEnabled"] is expected_forward_enabled:
        log.append(
            f"{label} buttons: back={expected_back_enabled}, forward={expected_forward_enabled}, "
            f"history={summarize_history_snapshot(snapshot)}"
        )
        return snapshot

    raise AssertionError(
        f"Expected {label} navigation buttons to be back={expected_back_enabled}, "
        f"forward={expected_forward_enabled}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_web_content_session_history_matches_ui(webdriver_port, session_id, label, log):
    snapshot = session_history(webdriver_port, session_id)
    ui = snapshot["ui"]
    if (
        ui["webContentHistoryMatchesUI"]
        and not ui["waitingToSeedWebContent"]
        and not ui["waitingForWebContentSeedAck"]
        and not ui["ignoringWebContentUpdatesUntilSeed"]
        and not ui["reseedAfterCurrentHistoryLoad"]
        and ui["pendingWebContentHistoryStepAfterFallbackLoad"] is None
        and ui["pendingSessionHistoryNavigation"] is None
        and ui["pendingSessionHistoryTraversal"] is None
        and comparable_history(ui) == comparable_history(snapshot["webContent"])
    ):
        log.append(f"{label} matched history: {summarize_history_snapshot(snapshot)}")
        return snapshot

    raise AssertionError(
        f"Expected {label} WebContent history to match UI, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_pending_session_history_traversal(
    webdriver_port,
    session_id,
    label,
    expected_entry_urls,
    expected_used_steps,
    expected_current_used_step_index,
    expected_back_enabled,
    expected_forward_enabled,
    expected_stage,
    expected_will_replace_web_content_process,
    log,
):
    snapshot = session_history(webdriver_port, session_id)
    ui = snapshot["ui"]
    pending_navigation = ui["pendingSessionHistoryNavigation"]
    pending_traversal = ui["pendingSessionHistoryTraversal"]

    log.append(f"{label} pending traversal: {summarize_history_snapshot(snapshot)}")

    if history_entry_urls(ui) != expected_entry_urls:
        raise AssertionError(f"Expected {label} entries to be {expected_entry_urls}\n" + "\n".join(log))

    if history_used_steps(ui) != expected_used_steps:
        raise AssertionError(f"Expected {label} used steps to be {expected_used_steps}\n" + "\n".join(log))

    if ui["currentUsedStepIndex"] != expected_current_used_step_index:
        raise AssertionError(
            f"Expected {label} current used step index to be {expected_current_used_step_index}\n" + "\n".join(log)
        )

    if (
        ui["backButtonEnabled"] is not expected_back_enabled
        or ui["forwardButtonEnabled"] is not expected_forward_enabled
    ):
        raise AssertionError(
            f"Expected {label} buttons to be back={expected_back_enabled}, forward={expected_forward_enabled}\n"
            + "\n".join(log)
        )

    if pending_traversal is None:
        raise AssertionError(f"Expected {label} to have a pending session history traversal\n" + "\n".join(log))

    if pending_navigation is None:
        raise AssertionError(f"Expected {label} to have a pending session history navigation\n" + "\n".join(log))

    if pending_navigation["url"] != expected_entry_urls[expected_current_used_step_index]:
        raise AssertionError(
            f"Expected {label} pending navigation URL to be "
            f"{expected_entry_urls[expected_current_used_step_index]}, got {pending_navigation['url']}\n"
            + "\n".join(log)
        )

    if pending_navigation["previousCurrentURL"] == pending_navigation["url"]:
        raise AssertionError(
            f"Expected {label} pending navigation to preserve the previous current URL\n" + "\n".join(log)
        )

    expected_step = expected_used_steps[expected_current_used_step_index]
    if pending_traversal["targetStep"] != expected_step:
        raise AssertionError(
            f"Expected {label} pending traversal target step to be {expected_step}, "
            f"got {pending_traversal['targetStep']}\n" + "\n".join(log)
        )

    if pending_traversal["targetStepIndex"] != expected_current_used_step_index:
        raise AssertionError(
            f"Expected {label} pending traversal target step index to be {expected_current_used_step_index}, "
            f"got {pending_traversal['targetStepIndex']}\n" + "\n".join(log)
        )

    if pending_traversal["stage"] != expected_stage:
        raise AssertionError(
            f"Expected {label} pending traversal stage to be {expected_stage}, "
            f"got {pending_traversal['stage']}\n" + "\n".join(log)
        )

    if pending_traversal["willReplaceWebContentProcess"] is not expected_will_replace_web_content_process:
        raise AssertionError(
            f"Expected {label} pending traversal willReplaceWebContentProcess to be "
            f"{expected_will_replace_web_content_process}, got "
            f"{pending_traversal['willReplaceWebContentProcess']}\n" + "\n".join(log)
        )

    expected_restore_mode = (
        "restore-from-ui-process" if expected_will_replace_web_content_process else "preserve-current-process-state"
    )
    if pending_navigation["webContentRestoreMode"] != expected_restore_mode:
        raise AssertionError(
            f"Expected {label} pending navigation restore mode to be {expected_restore_mode}, "
            f"got {pending_navigation['webContentRestoreMode']}\n" + "\n".join(log)
        )

    if ui["webContentHistoryMatchesUI"]:
        raise AssertionError(f"Expected {label} WebContent history to still be catching up\n" + "\n".join(log))

    return snapshot


def expect_current_entry_nested_history(webdriver_port, session_id, label, expected_url, expected_nested_urls, log):
    snapshot = session_history(webdriver_port, session_id)
    ui_current_entry = history_current_entry(snapshot["ui"])
    web_content_current_entry = history_current_entry(snapshot["webContent"])

    ui_nested_urls = []
    if ui_current_entry["nestedHistories"]:
        ui_nested_urls = [entry["url"] for entry in ui_current_entry["nestedHistories"][0]["entries"]]

    web_content_nested_urls = []
    if web_content_current_entry["nestedHistories"]:
        web_content_nested_urls = [entry["url"] for entry in web_content_current_entry["nestedHistories"][0]["entries"]]

    if (
        ui_current_entry["url"] == expected_url
        and web_content_current_entry["url"] == expected_url
        and ui_nested_urls == expected_nested_urls
        and web_content_nested_urls == expected_nested_urls
    ):
        log.append(f"{label} nested history: {expected_nested_urls}")
        return snapshot

    raise AssertionError(
        f"Expected {label} nested history to be {expected_nested_urls} at {expected_url}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_current_ui_entry_reload_pending(webdriver_port, session_id, label, expected_reload_pending, log):
    snapshot = session_history(webdriver_port, session_id)
    ui_current_entry = history_current_entry(snapshot["ui"])
    if ui_current_entry["reloadPending"] is expected_reload_pending:
        log.append(f"{label} UI reload pending: {expected_reload_pending}")
        return snapshot

    raise AssertionError(
        f"Expected {label} current UI entry reloadPending to be {expected_reload_pending}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def nested_history_urls_for_entry(history, url):
    matching_entries = [entry for entry in history["entries"] if entry["url"] == url]
    if len(matching_entries) > 1:
        raise AssertionError(f"Expected at most one entry for {url}, got {matching_entries}")
    if not matching_entries:
        return None

    entry = matching_entries[0]
    if not entry["nestedHistories"]:
        return []
    return [nested_entry["url"] for nested_entry in entry["nestedHistories"][0]["entries"]]


def expect_entry_nested_history(webdriver_port, session_id, label, entry_url, expected_nested_urls, log):
    snapshot = session_history(webdriver_port, session_id)
    ui_nested_urls = nested_history_urls_for_entry(snapshot["ui"], entry_url)
    web_content_nested_urls = nested_history_urls_for_entry(snapshot["webContent"], entry_url)
    if ui_nested_urls == expected_nested_urls and web_content_nested_urls == expected_nested_urls:
        log.append(f"{label} nested history: {expected_nested_urls}")
        return snapshot

    raise AssertionError(
        f"Expected {label} nested history to be {expected_nested_urls} at {entry_url}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_pending_web_content_history_step_after_fallback_load(webdriver_port, session_id, label, log):
    snapshot = session_history(webdriver_port, session_id)
    ui = snapshot["ui"]
    web_content = snapshot["webContent"]
    pending_step = ui["pendingWebContentHistoryStepAfterFallbackLoad"]
    pending_traversal = ui["pendingSessionHistoryTraversal"]
    ui_current_step = history_used_steps(ui)[ui["currentUsedStepIndex"]]
    web_content_current_step = history_used_steps(web_content)[web_content["currentUsedStepIndex"]]
    log.append(
        f"{label} pending fallback step: {pending_step}, "
        f"pendingTraversal={pending_traversal}, "
        f"webContentCurrentStep={ui['webContentCurrentStep']}, "
        f"matches={ui['webContentHistoryMatchesUI']}"
    )

    if pending_step is None:
        raise AssertionError(f"Expected {label} to have a pending fallback step\n" + "\n".join(log))

    if ui["webContentHistoryMatchesUI"]:
        raise AssertionError(
            f"Expected {label} WebContent history to be stale while step is pending\n" + "\n".join(log)
        )

    if ui["waitingToSeedWebContent"] or ui["waitingForWebContentSeedAck"] or ui["reseedAfterCurrentHistoryLoad"]:
        raise AssertionError(f"Expected {label} WebContent seed to be applied\n" + "\n".join(log))

    if pending_traversal is None:
        raise AssertionError(f"Expected {label} to have a pending session history traversal\n" + "\n".join(log))

    if pending_traversal["targetStep"] != pending_step:
        raise AssertionError(
            f"Expected {label} pending traversal target {pending_traversal['targetStep']} "
            f"to match pending WebContent step {pending_step}\n" + "\n".join(log)
        )

    if pending_traversal["stage"] != "restoring-nested-step-after-seed":
        raise AssertionError(
            f"Expected {label} pending traversal to be restoring nested step after seed, "
            f"got {pending_traversal['stage']}\n" + "\n".join(log)
        )

    if pending_step != ui_current_step:
        raise AssertionError(
            f"Expected {label} pending step {pending_step} to match UI current step {ui_current_step}\n"
            + "\n".join(log)
        )

    if ui["webContentCurrentStep"] != web_content_current_step:
        raise AssertionError(
            f"Expected {label} UI to report WebContent current step {web_content_current_step}, "
            f"got {ui['webContentCurrentStep']}\n" + "\n".join(log)
        )

    if history_current_step(ui["webContentKnownUsedSteps"]) != web_content_current_step:
        raise AssertionError(
            f"Expected {label} known WebContent steps to mark current step {web_content_current_step}\n"
            + "\n".join(log)
        )

    if ui["webContentCurrentStep"] == pending_step:
        raise AssertionError(
            f"Expected {label} WebContent to still be before pending step {pending_step}\n" + "\n".join(log)
        )


def expect_no_pending_web_content_history_step_after_fallback_load(webdriver_port, session_id, label, log):
    snapshot = session_history(webdriver_port, session_id)
    ui = snapshot["ui"]
    web_content = snapshot["webContent"]
    if (
        ui["pendingWebContentHistoryStepAfterFallbackLoad"] is None
        and ui["webContentHistoryMatchesUI"]
        and not ui["waitingToSeedWebContent"]
        and not ui["waitingForWebContentSeedAck"]
        and not ui["ignoringWebContentUpdatesUntilSeed"]
        and not ui["reseedAfterCurrentHistoryLoad"]
        and ui["pendingSessionHistoryNavigation"] is None
        and ui["pendingSessionHistoryTraversal"] is None
        and ui["webContentCurrentStep"] == history_used_steps(ui)[ui["currentUsedStepIndex"]]
        and history_current_step(ui["webContentKnownUsedSteps"]) == ui["webContentCurrentStep"]
        and comparable_history(ui) == comparable_history(web_content)
    ):
        log.append(f"{label} restored fallback step: {summarize_history_snapshot(snapshot)}")
        return

    raise AssertionError(
        f"Expected {label} to clear pending fallback step and match WebContent, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_url(webdriver_port, session_id, label, expected_url, log):
    actual_url = current_url(webdriver_port, session_id)
    if actual_url == expected_url:
        log.append(f"{label}: {actual_url}")
        return

    raise AssertionError(f"Expected {label} to be {expected_url}, got {actual_url}\n" + "\n".join(log))


def wait_for_url(webdriver_port, session_id, label, expected_url, log, timeout=EVENT_TIMEOUT_SECONDS):
    deadline = time.monotonic() + timeout
    actual_url = None
    while time.monotonic() < deadline:
        actual_url = current_url(webdriver_port, session_id)
        if actual_url == expected_url:
            log.append(f"{label}: {actual_url}")
            return
        time.sleep(0.05)

    raise AssertionError(f"Timed out waiting for {label} to be {expected_url}, got {actual_url}\n" + "\n".join(log))


def expect_body_text(webdriver_port, session_id, label, expected_text, log):
    actual = execute_script(webdriver_port, session_id, "return [document.body.innerText.trim(), document.readyState];")
    if actual == [expected_text, "complete"]:
        log.append(f"{label}: {actual[0]}")
        return

    raise AssertionError(f"Expected {label} body text to be {expected_text}, got {actual}\n" + "\n".join(log))


def expect_current_entry_resource(webdriver_port, session_id, label, expected_resource, log):
    snapshot = session_history(webdriver_port, session_id)
    ui_current_entry = history_current_entry(snapshot["ui"])
    web_content_current_entry = history_current_entry(snapshot["webContent"])
    if ui_current_entry["resource"] == expected_resource and web_content_current_entry["resource"] == expected_resource:
        log.append(f"{label} current resource: {expected_resource}")
        return snapshot

    raise AssertionError(
        f"Expected {label} current resource to be {expected_resource}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_current_ui_entry_resource(webdriver_port, session_id, label, expected_resource, log):
    snapshot = session_history(webdriver_port, session_id)
    ui_current_entry = history_current_entry(snapshot["ui"])
    if ui_current_entry["resource"] == expected_resource:
        log.append(f"{label} current UI resource: {expected_resource}")
        return snapshot

    raise AssertionError(
        f"Expected {label} current UI resource to be {expected_resource}, "
        f"got {summarize_history_snapshot(snapshot)}\n" + "\n".join(log)
    )


def expect_frame_url(webdriver_port, session_id, label, expected_url, log, expected_title=None):
    result = execute_script(
        webdriver_port,
        session_id,
        """
const frame = document.getElementById('frame');
return [frame.contentWindow.location.href, frame.contentDocument.readyState, frame.contentDocument.title];
""",
    )
    actual_url, ready_state, actual_title = result
    if (
        actual_url == expected_url
        and ready_state == "complete"
        and (expected_title is None or actual_title == expected_title)
    ):
        log.append(f"{label}: {actual_url}")
        return

    actual = f"{actual_url} ({actual_title})"
    raise AssertionError(f"Expected {label} to be {expected_url}, got {actual}\n" + "\n".join(log))


def expect_history_entry_state(
    webdriver_port,
    session_id,
    label,
    expected_url,
    expected_classic_state,
    expected_navigation_state,
    expected_scroll_restoration,
    expected_navigation_key,
    expected_navigation_id,
    log,
):
    result = execute_script(
        webdriver_port,
        session_id,
        """
return [
    location.href,
    JSON.stringify(history.state),
    JSON.stringify(navigation.currentEntry.getState()),
    history.scrollRestoration,
    navigation.currentEntry.key,
    navigation.currentEntry.id,
];
""",
    )

    log.append(f"{label}: {result}")
    expected = [
        expected_url,
        expected_classic_state,
        expected_navigation_state,
        expected_scroll_restoration,
        expected_navigation_key,
        expected_navigation_id,
    ]
    if result != expected:
        raise AssertionError(f"Expected {label} to be {expected}, got {result}\n" + "\n".join(log))


def expect_window_name(webdriver_port, session_id, label, expected_name, log):
    actual_name = execute_script(webdriver_port, session_id, "return window.name;")
    log.append(f"{label}: {actual_name}")
    if actual_name != expected_name:
        raise AssertionError(f"Expected {label} to be {expected_name}, got {actual_name}\n" + "\n".join(log))


def expect_scroll_position(webdriver_port, session_id, label, expected_x, expected_y, log):
    actual = execute_script(
        webdriver_port,
        session_id,
        "return [Math.round(scrollX), Math.round(scrollY), document.readyState];",
    )
    if actual == [expected_x, expected_y, "complete"]:
        log.append(f"{label}: {actual}")
        return

    raise AssertionError(
        f"Expected {label} to be {[expected_x, expected_y, 'complete']}, got {actual}\n" + "\n".join(log)
    )


def run_blocked_process_swap_ui_forward_crash_recovery_test(
    webdriver_port,
    page_server,
    url_b,
    url_process_swap_back_blocked,
):
    session_id = create_session(webdriver_port)
    log = [f"blocked process-swap UI forward crash recovery initial: {current_url(webdriver_port, session_id)}"]
    with page_server.process_swap_back_blocked_request_lock:
        page_server.process_swap_back_blocked_request_count = 0
    page_server.blocked_process_swap_back_requested.clear()
    page_server.process_swap_back_recovery_requested.clear()
    page_server.release_blocked_process_swap_back.clear()
    load_url_from_ui(webdriver_port, session_id, url_b)
    expect_url(webdriver_port, session_id, "after process-swap forward crash recovery setup /b", url_b, log)
    load_url_from_ui(webdriver_port, session_id, url_process_swap_back_blocked)
    expect_url(
        webdriver_port,
        session_id,
        "after process-swap forward crash recovery setup /process-swap-back-blocked",
        url_process_swap_back_blocked,
        log,
    )
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after process-swap forward crash recovery setup",
        [url_b, url_process_swap_back_blocked],
        [0, 1],
        1,
        True,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1],
        expected_web_content_current_step=1,
    )

    traverse_history_from_ui(webdriver_port, session_id, -1)
    expect_url(webdriver_port, session_id, "after process-swap forward crash recovery setup back to /b", url_b, log)
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after process-swap forward crash recovery setup back to /b",
        [url_b, url_process_swap_back_blocked],
        [0, 1],
        0,
        False,
        True,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1],
        expected_web_content_current_step=0,
    )

    page_server.blocked_process_swap_back_requested.clear()
    page_server.process_swap_back_recovery_requested.clear()
    page_server.release_blocked_process_swap_back.clear()
    page_server.process_swap_back_document_ran.clear()
    traverse_history_from_ui(webdriver_port, session_id, 1, wait_for_navigation_completion=False)
    wait_for_event(
        page_server.blocked_process_swap_back_requested,
        "blocked process-swap UI forward before crash",
    )
    expect_pending_session_history_traversal(
        webdriver_port,
        session_id,
        "while process-swap UI forward loads target before crash",
        [url_b, url_process_swap_back_blocked],
        [0, 1],
        1,
        True,
        False,
        "replacing-webcontent-process",
        True,
        log,
    )
    crash_current_page_allowing_navigation_timeout(webdriver_port, session_id)
    wait_for_event(page_server.process_swap_back_recovery_requested, "process-swap forward recovery request")
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "while process-swap UI forward recovers target after crash",
        [url_b, url_process_swap_back_blocked],
        [0, 1],
        1,
        True,
        False,
        log,
        expect_web_content_matches_ui=False,
        expected_waiting_to_seed_web_content=True,
        expected_waiting_for_web_content_seed_ack=False,
        expected_ignoring_web_content_updates_until_seed=True,
        expected_reseed_after_current_history_load=True,
    )
    page_server.release_blocked_process_swap_back.set()
    wait_for_event(page_server.process_swap_back_document_ran, "process-swap UI forward recovery document")
    expect_url(
        webdriver_port,
        session_id,
        "after process-swap UI forward crash recovery completes",
        url_process_swap_back_blocked,
        log,
    )
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after process-swap UI forward crash recovery completes",
        [url_b, url_process_swap_back_blocked],
        [0, 1],
        1,
        True,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1],
        expected_web_content_current_step=1,
    )
    request(webdriver_port, "DELETE", f"/session/{session_id}")


def expect_second_ui_forward_during_pending_forward_does_not_hang(
    webdriver_port,
    session_id,
    page_server,
    url_a,
    url_forward_blocked,
    url_c,
):
    log = [f"second UI forward during pending forward initial: {current_url(webdriver_port, session_id)}"]
    page_server.block_forward_load = False
    page_server.blocked_forward_requested.clear()
    page_server.release_blocked_forward.clear()

    load_url_from_ui(webdriver_port, session_id, url_a)
    expect_url(webdriver_port, session_id, "after pending-forward setup /a", url_a, log)
    load_url_from_ui(webdriver_port, session_id, url_forward_blocked)
    expect_url(webdriver_port, session_id, "after pending-forward setup /forward-blocked", url_forward_blocked, log)
    load_url_from_ui(webdriver_port, session_id, url_c)
    expect_url(webdriver_port, session_id, "after pending-forward setup /c", url_c, log)
    expect_web_content_session_history_matches_ui(webdriver_port, session_id, "after pending-forward setup", log)

    traverse_history_from_ui(webdriver_port, session_id, -1)
    expect_url(
        webdriver_port, session_id, "after pending-forward setup back to /forward-blocked", url_forward_blocked, log
    )
    traverse_history_from_ui(webdriver_port, session_id, -1)
    expect_url(webdriver_port, session_id, "after pending-forward setup back to /a", url_a, log)

    page_server.block_forward_load = True
    page_server.blocked_forward_requested.clear()
    page_server.release_blocked_forward.clear()
    traverse_history_from_ui(webdriver_port, session_id, 1, wait_for_navigation_completion=False)
    wait_for_event(page_server.blocked_forward_requested, "blocked first UI forward")

    second_forward_error = []

    def request_second_forward():
        try:
            traverse_history_from_ui(webdriver_port, session_id, 1)
        except Exception as error:
            second_forward_error.append(error)

    second_forward_thread = threading.Thread(target=request_second_forward)
    second_forward_thread.start()
    page_server.release_blocked_forward.set()
    second_forward_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
    if second_forward_thread.is_alive():
        raise AssertionError("Timed out waiting for second UI forward during pending forward\n" + "\n".join(log))
    if second_forward_error:
        raise AssertionError(
            f"Second UI forward during pending forward failed: {second_forward_error[0]}\n" + "\n".join(log)
        )
    expect_url(webdriver_port, session_id, "after second UI forward during pending forward", url_c, log)
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after second UI forward during pending forward",
        [url_a, url_forward_blocked, url_c],
        [0, 1, 2],
        2,
        True,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1, 2],
        expected_web_content_current_step=2,
    )
    page_server.block_forward_load = False


def expect_sandboxed_history_back_to_be_blocked(webdriver_port, session_id, url_a, url_b, log):
    sandboxed_history_back_state = execute_async_script(
        webdriver_port,
        session_id,
        """
const done = arguments[0];
window.sandboxedHistoryBackResult = null;
window.addEventListener("message", event => {
    if (event.data.startsWith("sandboxed-history-back:")) {
        window.sandboxedHistoryBackResult = event.data;
        done([location.href, window.sandboxedHistoryBackResult]);
    }
}, { once: true });
const iframe = document.createElement("iframe");
iframe.sandbox = "allow-same-origin allow-scripts";
iframe.srcdoc = `
<script>
try {
    history.back();
    parent.postMessage("sandboxed-history-back:called", "*");
} catch (e) {
    parent.postMessage("sandboxed-history-back:threw:" + e.name, "*");
}
</scr` + `ipt>`;
document.body.append(iframe);
""",
    )
    if sandboxed_history_back_state != [url_b, "sandboxed-history-back:called"]:
        raise AssertionError(
            f"Expected sandboxed iframe history.back() to be blocked, got {sandboxed_history_back_state}\n"
            + "\n".join(log)
        )

    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after sandboxed iframe history.back() from /b",
        [url_a, url_b],
        [0, 1],
        1,
        True,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1],
        expected_web_content_current_step=1,
    )
    execute_script(webdriver_port, session_id, "document.querySelector('iframe')?.remove(); return null;")


def expect_beforeunload_cancels_stale_ui_load(
    webdriver_port,
    session_id,
    url_b,
    url_c,
    before_blocked_browser_ui_back,
    blocked_link_navigation_state,
    log,
):
    mark_web_content_session_history_stale(webdriver_port, session_id)
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after marking WebContent session history stale",
        history_entry_urls(before_blocked_browser_ui_back["ui"]),
        history_used_steps(before_blocked_browser_ui_back["ui"]),
        before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
        before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
        before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
        log,
        expect_web_content_matches_ui=False,
        expected_web_content_known_used_steps=history_step_values(
            before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
        ),
        expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
    )

    load_url_from_ui(webdriver_port, session_id, url_c)
    blocked_stale_ui_load_state = execute_script(
        webdriver_port,
        session_id,
        "return [location.href, window.beforeUnloadCount];",
    )
    if blocked_stale_ui_load_state[0] != url_b or blocked_stale_ui_load_state[1] <= blocked_link_navigation_state[1]:
        raise AssertionError(
            f"Expected beforeunload to cancel stale-history UI load from /b, got {blocked_stale_ui_load_state}\n"
            + "\n".join(log)
        )

    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after blocked stale-history UI load from /b",
        history_entry_urls(before_blocked_browser_ui_back["ui"]),
        history_used_steps(before_blocked_browser_ui_back["ui"]),
        before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
        before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
        before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=history_step_values(
            before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
        ),
        expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
    )

    return blocked_stale_ui_load_state


def expect_same_url_ui_load_replaces_current_entry(webdriver_port, session_id, url, log):
    load_url_from_ui(webdriver_port, session_id, url)
    expect_url(webdriver_port, session_id, "after same-URL UI load setup /a", url, log)
    load_url_from_ui(webdriver_port, session_id, url)
    expect_url(webdriver_port, session_id, "after same-URL UI load /a", url, log)
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after same-URL UI load /a",
        [url],
        [0],
        0,
        False,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0],
        expected_web_content_current_step=0,
    )


def expect_webdriver_fragment_navigation_completes(webdriver_port, session_id, url, log):
    fragment_url = f"{url}#fragment"
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 1000})
    request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": fragment_url})
    request(webdriver_port, "POST", f"/session/{session_id}/timeouts", {"pageLoad": 10000})
    expect_url(webdriver_port, session_id, "after WebDriver fragment navigation", fragment_url, log)
    expect_ui_session_history(
        webdriver_port,
        session_id,
        "after WebDriver fragment navigation",
        [url, fragment_url],
        [0, 1],
        1,
        True,
        False,
        log,
        expect_web_content_matches_ui=True,
        expected_web_content_known_used_steps=[0, 1],
        expected_web_content_current_step=1,
    )


def run_webdriver_fragment_navigation_test(webdriver_port, url):
    session_id = create_session(webdriver_port)
    log = [f"WebDriver fragment navigation initial: {current_url(webdriver_port, session_id)}"]
    load_url_from_ui(webdriver_port, session_id, url)
    expect_url(webdriver_port, session_id, "after WebDriver fragment navigation setup /a", url, log)
    expect_webdriver_fragment_navigation_completes(webdriver_port, session_id, url, log)
    request(webdriver_port, "DELETE", f"/session/{session_id}")


def run_test(webdriver_binary):
    page_server = TestPageServer(("0.0.0.0", 0), TestPageHandler)
    page_server_thread = threading.Thread(target=page_server.serve_forever, daemon=True)
    page_server_thread.start()

    webdriver_port = unused_port()
    env = os.environ.copy()
    env["LADYBIRD_WEBDRIVER_ENABLE_SITE_ISOLATION"] = "1"
    env["LADYBIRD_SESSION_HISTORY_DEBUG"] = "1"

    webdriver_stdout = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    webdriver_stderr = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    expect_reload_pending_history_log = False
    expect_no_entry_history_log = False

    webdriver = subprocess.Popen(
        [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(webdriver_port)],
        stdout=webdriver_stdout,
        stderr=webdriver_stderr,
        text=True,
        env=env,
    )

    session_id = None
    failed = False
    try:
        wait_for_port(webdriver_port)

        baseline_open_fds = count_open_fds(webdriver.pid)
        page_port = page_server.server_port
        url_a = f"http://localhost:{page_port}/a"
        url_b = f"http://127.0.0.1:{page_port}/b"
        url_c = f"http://127.0.0.1:{page_port}/c"
        url_d = f"http://localhost:{page_port}/d"
        url_redirect_to_b = f"http://localhost:{page_port}/redirect-to-b"
        url_nested = f"http://localhost:{page_port}/nested"
        url_state = f"http://localhost:{page_port}/state"
        url_state_replace_load = f"http://localhost:{page_port}/state?replace-load"
        url_state_replace = f"http://localhost:{page_port}/state?replace"
        url_state_push = f"http://localhost:{page_port}/state?push"
        url_scroll = f"http://localhost:{page_port}/scroll"
        url_scroll_saved = f"http://localhost:{page_port}/scroll?saved"
        url_reload_blocked = f"http://localhost:{page_port}/reload-blocked"
        url_process_swap_back_blocked = f"http://localhost:{page_port}/process-swap-back-blocked"
        url_forward_blocked = f"http://127.0.0.1:{page_port}/forward-blocked"
        url_frame_a = f"http://localhost:{page_port}/frame-a"
        url_frame_b_blocked = f"http://localhost:{page_port}/frame-b-blocked"
        url_nested_same_document = f"http://localhost:{page_port}/nested?same-document"
        url_post_form = f"http://localhost:{page_port}/post-form"
        url_post_result = f"http://localhost:{page_port}/post-result"
        url_post_blocked_form = f"http://localhost:{page_port}/post-blocked-form"
        url_post_blocked_result = f"http://localhost:{page_port}/post-result-blocked-same-site"
        url_post_same_url_blocked = f"http://localhost:{page_port}/post-same-url-blocked"
        url_cross_site_post_form = f"http://localhost:{page_port}/cross-site-post-form"
        url_cross_site_post_blocked_form = f"http://localhost:{page_port}/cross-site-post-blocked-form"
        url_cross_site_post_blocked_result = f"http://127.0.0.1:{page_port}/post-result-blocked"
        url_cross_site_post_result = f"http://127.0.0.1:{page_port}/post-result"

        expect_ladybird_test_hooks_require_capability(webdriver_port)

        session_id = create_session(webdriver_port)
        expect_second_ui_forward_during_pending_forward_does_not_hang(
            webdriver_port,
            session_id,
            page_server,
            url_a,
            url_forward_blocked,
            url_c,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"first-entry replace initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_a)
        expect_url(webdriver_port, session_id, "after first-entry replace test /a", url_a, log)
        page_server.state_replace_load_document_ran.clear()
        execute_script(
            webdriver_port, session_id, f"location.replace({json.dumps(url_state_replace_load)}); return null;"
        )
        wait_for_event(page_server.state_replace_load_document_ran, "state replace-load document script")
        expect_url(
            webdriver_port,
            session_id,
            "after first-entry location.replace() to /state?replace-load",
            url_state_replace_load,
            log,
        )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after first-entry location.replace() to /state?replace-load",
            [url_state_replace_load],
            [0],
            0,
            False,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0],
            expected_web_content_current_step=0,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"same-URL UI load initial: {current_url(webdriver_port, session_id)}"]
        expect_same_url_ui_load_replaces_current_entry(webdriver_port, session_id, url_a, log)
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        run_webdriver_fragment_navigation_test(webdriver_port, url_a)

        session_id = create_session(webdriver_port)
        log = [f"duplicate URL crash recovery initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_a)
        expect_url(webdriver_port, session_id, "after duplicate URL setup /a", url_a, log)
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after first duplicate URL setup /b", url_b, log)
        load_url_from_ui(webdriver_port, session_id, url_c)
        expect_url(webdriver_port, session_id, "after duplicate URL setup /c", url_c, log)
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after second duplicate URL setup /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL setup",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            3,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=3,
        )

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after duplicate URL crash recovery", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL crash recovery",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            3,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=3,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after duplicate URL crash recovery back", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL crash recovery back",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            2,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=2,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after duplicate URL crash recovery second back", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL crash recovery second back",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=1,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after duplicate URL crash recovery third back", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL crash recovery third back",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=0,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_no_entry_history_log = True
        expect_url(webdriver_port, session_id, "after duplicate URL crash recovery back past start", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after duplicate URL crash recovery back past start",
            [url_a, url_b, url_c, url_b],
            [0, 1, 2, 3],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2, 3],
            expected_web_content_current_step=0,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked cross-site POST process swap initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_cross_site_post_blocked_form)
        expect_url(
            webdriver_port,
            session_id,
            "after blocked cross-site POST form load",
            url_cross_site_post_blocked_form,
            log,
        )
        post_form_submit_point = execute_script(
            webdriver_port,
            session_id,
            """
const button = document.getElementById('submit');
const rect = button.getBoundingClientRect();
return [Math.round(rect.left + rect.width / 2), Math.round(rect.top + rect.height / 2)];
""",
        )
        page_server.blocked_cross_site_post_requested.clear()
        page_server.release_blocked_cross_site_post.clear()
        page_server.cross_site_post_result_document_ran.clear()

        def request_blocked_cross_site_post():
            perform_pointer_click(webdriver_port, session_id, post_form_submit_point[0], post_form_submit_point[1], log)

        blocked_cross_site_post_thread = threading.Thread(target=request_blocked_cross_site_post)
        blocked_cross_site_post_thread.start()
        wait_for_event(page_server.blocked_cross_site_post_requested, "blocked cross-site POST")
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while cross-site POST process swap is blocked",
            [url_cross_site_post_blocked_form, url_cross_site_post_blocked_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=False,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )
        expect_current_ui_entry_resource(
            webdriver_port,
            session_id,
            "while cross-site POST process swap is blocked",
            "post",
            log,
        )
        page_server.release_blocked_cross_site_post.set()
        blocked_cross_site_post_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
        if blocked_cross_site_post_thread.is_alive():
            raise AssertionError("Timed out waiting for blocked cross-site POST request to finish\n" + "\n".join(log))
        wait_for_event(page_server.cross_site_post_result_document_ran, "blocked cross-site POST result document")
        expect_url(
            webdriver_port,
            session_id,
            "after blocked cross-site POST form submit",
            url_cross_site_post_blocked_result,
            log,
        )
        expect_body_text(
            webdriver_port, session_id, "after blocked cross-site POST form submit", "POST:name=ladybird", log
        )
        expect_current_entry_resource(
            webdriver_port, session_id, "after blocked cross-site POST form submit", "post", log
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"cross-site POST process swap initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_cross_site_post_form)
        expect_url(webdriver_port, session_id, "after cross-site POST form load", url_cross_site_post_form, log)
        post_form_submit_point = execute_script(
            webdriver_port,
            session_id,
            """
const button = document.getElementById('submit');
const rect = button.getBoundingClientRect();
return [Math.round(rect.left + rect.width / 2), Math.round(rect.top + rect.height / 2)];
""",
        )
        page_server.post_result_document_ran.clear()
        perform_pointer_click(webdriver_port, session_id, post_form_submit_point[0], post_form_submit_point[1], log)
        wait_for_event(page_server.post_result_document_ran, "cross-site POST result document")
        expect_url(webdriver_port, session_id, "after cross-site POST form submit", url_cross_site_post_result, log)
        expect_body_text(webdriver_port, session_id, "after cross-site POST form submit", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after cross-site POST form submit", "post", log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after cross-site POST form submit",
            [url_cross_site_post_form, url_cross_site_post_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked process-swap UI back initial: {current_url(webdriver_port, session_id)}"]
        with page_server.process_swap_back_blocked_request_lock:
            page_server.process_swap_back_blocked_request_count = 0
        page_server.blocked_process_swap_back_requested.clear()
        page_server.release_blocked_process_swap_back.clear()
        load_url_from_ui(webdriver_port, session_id, url_process_swap_back_blocked)
        expect_url(
            webdriver_port,
            session_id,
            "after blocked process-swap setup /process-swap-back-blocked",
            url_process_swap_back_blocked,
            log,
        )
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after blocked process-swap setup /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap setup",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        page_server.process_swap_back_document_ran.clear()
        traverse_history_from_ui(webdriver_port, session_id, -1, wait_for_navigation_completion=False)
        wait_for_event(page_server.blocked_process_swap_back_requested, "blocked process-swap UI back")
        expect_pending_session_history_traversal(
            webdriver_port,
            session_id,
            "while process-swap UI back loads target",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            "replacing-webcontent-process",
            True,
            log,
        )
        page_server.release_blocked_process_swap_back.set()
        wait_for_event(page_server.process_swap_back_document_ran, "process-swap UI back document")
        wait_for_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap UI back converges",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        expect_url(
            webdriver_port,
            session_id,
            "after blocked process-swap UI back completes",
            url_process_swap_back_blocked,
            log,
        )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap UI back completes",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked process-swap WebDriver back initial: {current_url(webdriver_port, session_id)}"]
        with page_server.process_swap_back_blocked_request_lock:
            page_server.process_swap_back_blocked_request_count = 0
        page_server.blocked_process_swap_back_requested.clear()
        page_server.release_blocked_process_swap_back.clear()
        load_url_from_ui(webdriver_port, session_id, url_process_swap_back_blocked)
        expect_url(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver setup /process-swap-back-blocked",
            url_process_swap_back_blocked,
            log,
        )
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after blocked process-swap WebDriver setup /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver setup",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        webdriver_back_error = []

        def request_webdriver_back():
            try:
                request(webdriver_port, "POST", f"/session/{session_id}/back", {})
            except Exception as error:
                webdriver_back_error.append(error)

        webdriver_back_thread = threading.Thread(target=request_webdriver_back)
        page_server.process_swap_back_document_ran.clear()
        webdriver_back_thread.start()
        wait_for_event(page_server.blocked_process_swap_back_requested, "blocked process-swap WebDriver back")
        page_server.release_blocked_process_swap_back.set()
        wait_for_event(page_server.process_swap_back_document_ran, "process-swap WebDriver back document")
        webdriver_back_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
        if webdriver_back_thread.is_alive():
            raise AssertionError("Timed out waiting for WebDriver back request to finish\n" + "\n".join(log))
        if webdriver_back_error:
            raise webdriver_back_error[0]
        expect_url(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver back completes",
            url_process_swap_back_blocked,
            log,
        )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver back completes",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked process-swap WebDriver back beforeunload initial: {current_url(webdriver_port, session_id)}"]
        with page_server.process_swap_back_blocked_request_lock:
            page_server.process_swap_back_blocked_request_count = 0
        page_server.blocked_process_swap_back_requested.clear()
        page_server.release_blocked_process_swap_back.clear()
        load_url_from_ui(webdriver_port, session_id, url_process_swap_back_blocked)
        expect_url(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver beforeunload setup /process-swap-back-blocked",
            url_process_swap_back_blocked,
            log,
        )
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after blocked process-swap WebDriver beforeunload setup /b", url_b, log)
        before_blocked_process_swap_webdriver_back = expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "before blocked process-swap WebDriver back from /b", log
        )
        beforeunload_setup = execute_script(
            webdriver_port,
            session_id,
            """
window.beforeUnloadCount = 0;
window.onbeforeunload = event => {
    ++window.beforeUnloadCount;
    event.preventDefault();
    event.returnValue = "blocked";
    return "blocked";
};
return [location.href, window.beforeUnloadCount];
""",
        )
        if beforeunload_setup != [url_b, 0]:
            raise AssertionError(
                f"Expected process-swap WebDriver beforeunload setup on /b to be {[url_b, 0]}, "
                f"got {beforeunload_setup}\n" + "\n".join(log)
            )
        inert_click_point = execute_script(
            webdriver_port,
            session_id,
            """
const rect = document.querySelector("p").getBoundingClientRect();
return [Math.floor(rect.left + rect.width / 2), Math.floor(rect.top + rect.height / 2)];
""",
        )
        perform_pointer_click(webdriver_port, session_id, inert_click_point[0], inert_click_point[1], log)

        webdriver_back_error = []

        def request_blocked_webdriver_back():
            try:
                request(webdriver_port, "POST", f"/session/{session_id}/back", {})
            except Exception as error:
                webdriver_back_error.append(error)

        webdriver_back_thread = threading.Thread(target=request_blocked_webdriver_back)
        webdriver_back_thread.start()
        webdriver_back_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
        if page_server.blocked_process_swap_back_requested.is_set():
            page_server.release_blocked_process_swap_back.set()
            webdriver_back_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
            raise AssertionError(
                "Expected beforeunload to cancel process-swap WebDriver back before loading target\n" + "\n".join(log)
            )
        if webdriver_back_thread.is_alive():
            raise AssertionError(
                "Timed out waiting for blocked process-swap WebDriver back request to finish\n" + "\n".join(log)
            )
        if webdriver_back_error:
            raise webdriver_back_error[0]
        blocked_process_swap_webdriver_back_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if blocked_process_swap_webdriver_back_state != [url_b, 1]:
            raise AssertionError(
                f"Expected beforeunload to cancel process-swap WebDriver back from /b, "
                f"got {blocked_process_swap_webdriver_back_state}\n" + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked process-swap WebDriver back from /b",
            history_entry_urls(before_blocked_process_swap_webdriver_back["ui"]),
            history_used_steps(before_blocked_process_swap_webdriver_back["ui"]),
            before_blocked_process_swap_webdriver_back["ui"]["currentUsedStepIndex"],
            before_blocked_process_swap_webdriver_back["ui"]["backButtonEnabled"],
            before_blocked_process_swap_webdriver_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_process_swap_webdriver_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_process_swap_webdriver_back["ui"]["webContentCurrentStep"],
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked process-swap UI back crash recovery initial: {current_url(webdriver_port, session_id)}"]
        with page_server.process_swap_back_blocked_request_lock:
            page_server.process_swap_back_blocked_request_count = 0
        page_server.blocked_process_swap_back_requested.clear()
        page_server.process_swap_back_recovery_requested.clear()
        page_server.release_blocked_process_swap_back.clear()
        load_url_from_ui(webdriver_port, session_id, url_process_swap_back_blocked)
        expect_url(
            webdriver_port,
            session_id,
            "after process-swap crash recovery setup /process-swap-back-blocked",
            url_process_swap_back_blocked,
            log,
        )
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after process-swap crash recovery setup /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after process-swap crash recovery setup",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        page_server.process_swap_back_document_ran.clear()
        traverse_history_from_ui(webdriver_port, session_id, -1, wait_for_navigation_completion=False)
        wait_for_event(
            page_server.blocked_process_swap_back_requested,
            "blocked process-swap UI back before crash",
        )
        expect_pending_session_history_traversal(
            webdriver_port,
            session_id,
            "while process-swap UI back loads target before crash",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            "replacing-webcontent-process",
            True,
            log,
        )
        crash_current_page_allowing_navigation_timeout(webdriver_port, session_id)
        wait_for_event(page_server.process_swap_back_recovery_requested, "process-swap back recovery request")
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while process-swap UI back recovers target after crash",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=False,
            expected_waiting_to_seed_web_content=True,
            expected_waiting_for_web_content_seed_ack=False,
            expected_ignoring_web_content_updates_until_seed=True,
            expected_reseed_after_current_history_load=True,
        )
        page_server.release_blocked_process_swap_back.set()
        wait_for_event(page_server.process_swap_back_document_ran, "process-swap UI back recovery document")
        expect_url(
            webdriver_port,
            session_id,
            "after process-swap UI back crash recovery completes",
            url_process_swap_back_blocked,
            log,
        )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after process-swap UI back crash recovery completes",
            [url_process_swap_back_blocked, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        run_blocked_process_swap_ui_forward_crash_recovery_test(
            webdriver_port,
            page_server,
            url_b,
            url_process_swap_back_blocked,
        )

        session_id = create_session(webdriver_port)
        log = [f"nested restore crash recovery initial: {current_url(webdriver_port, session_id)}"]
        page_server.release_blocked_frame_b.set()
        page_server.blocked_frame_b_requested.clear()
        load_url_from_ui(webdriver_port, session_id, url_nested)
        expect_url(webdriver_port, session_id, "after nested restore crash setup /nested", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested restore crash setup initial frame",
            url_frame_a,
            log,
            "Frame A",
        )
        page_server.frame_b_blocked_document_ran.clear()
        execute_script(
            webdriver_port,
            session_id,
            f"document.getElementById('frame').contentWindow.location.href = '{url_frame_b_blocked}'; return null;",
        )
        wait_for_event(page_server.frame_b_blocked_document_ran, "nested frame setup document")
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested restore crash setup frame navigation",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested restore crash setup",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        load_url_from_ui(webdriver_port, session_id, url_b)
        expect_url(webdriver_port, session_id, "after nested restore crash setup /b", url_b, log)
        expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "before nested restore crash recovery", log
        )

        page_server.release_blocked_frame_b.clear()
        page_server.blocked_frame_b_requested.clear()
        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "while nested restore crash recovery loads /nested", url_nested, log)
        wait_for_event(page_server.blocked_frame_b_requested, "blocked nested frame restore before crash")
        expect_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "while nested restore crash recovery waits for frame", log
        )
        crash_current_page_allowing_navigation_timeout(webdriver_port, session_id)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while nested restore crash recovery reloads current entry",
            [url_nested, url_b],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=False,
        )
        page_server.frame_b_blocked_document_ran.clear()
        page_server.release_blocked_frame_b.set()
        wait_for_event(page_server.frame_b_blocked_document_ran, "nested frame restore document")
        expect_url(webdriver_port, session_id, "after nested restore crash recovery", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested restore crash recovery restores iframe",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested restore crash recovery",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_no_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "after nested restore crash recovery", log
        )
        expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "after nested restore crash recovery", log
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked same-site POST initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_post_blocked_form)
        expect_url(webdriver_port, session_id, "after blocked same-site POST form load", url_post_blocked_form, log)
        post_form_submit_point = execute_script(
            webdriver_port,
            session_id,
            """
const button = document.getElementById('submit');
const rect = button.getBoundingClientRect();
return [Math.round(rect.left + rect.width / 2), Math.round(rect.top + rect.height / 2)];
""",
        )
        page_server.blocked_same_site_post_requested.clear()
        page_server.release_blocked_same_site_post.clear()
        page_server.same_site_post_result_document_ran.clear()

        def request_blocked_same_site_post():
            perform_pointer_click(webdriver_port, session_id, post_form_submit_point[0], post_form_submit_point[1], log)

        blocked_same_site_post_thread = threading.Thread(target=request_blocked_same_site_post)
        blocked_same_site_post_thread.start()
        wait_for_event(page_server.blocked_same_site_post_requested, "blocked same-site POST")
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while same-site POST is blocked",
            [url_post_blocked_form, url_post_blocked_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=False,
        )
        expect_current_ui_entry_resource(
            webdriver_port,
            session_id,
            "while same-site POST is blocked",
            "post",
            log,
        )
        page_server.release_blocked_same_site_post.set()
        blocked_same_site_post_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
        if blocked_same_site_post_thread.is_alive():
            raise AssertionError("Timed out waiting for blocked same-site POST request to finish\n" + "\n".join(log))
        wait_for_event(page_server.same_site_post_result_document_ran, "blocked same-site POST result document")
        expect_url(webdriver_port, session_id, "after blocked same-site POST submit", url_post_blocked_result, log)
        expect_body_text(webdriver_port, session_id, "after blocked same-site POST submit", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after blocked same-site POST submit", "post", log)
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"blocked same-URL POST initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_post_same_url_blocked)
        expect_url(webdriver_port, session_id, "after blocked same-URL POST form load", url_post_same_url_blocked, log)
        post_form_submit_point = execute_script(
            webdriver_port,
            session_id,
            """
const button = document.getElementById('submit');
const rect = button.getBoundingClientRect();
return [Math.round(rect.left + rect.width / 2), Math.round(rect.top + rect.height / 2)];
""",
        )
        page_server.blocked_same_url_post_requested.clear()
        page_server.release_blocked_same_url_post.clear()
        page_server.same_url_post_result_document_ran.clear()

        def request_blocked_same_url_post():
            perform_pointer_click(webdriver_port, session_id, post_form_submit_point[0], post_form_submit_point[1], log)

        blocked_same_url_post_thread = threading.Thread(target=request_blocked_same_url_post)
        blocked_same_url_post_thread.start()
        wait_for_event(page_server.blocked_same_url_post_requested, "blocked same-URL POST")
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while same-URL POST is blocked",
            [url_post_same_url_blocked],
            [0],
            0,
            False,
            False,
            log,
            expect_web_content_matches_ui=False,
        )
        expect_current_ui_entry_resource(
            webdriver_port,
            session_id,
            "while same-URL POST is blocked",
            "post",
            log,
        )
        page_server.release_blocked_same_url_post.set()
        blocked_same_url_post_thread.join(timeout=EVENT_TIMEOUT_SECONDS)
        if blocked_same_url_post_thread.is_alive():
            raise AssertionError("Timed out waiting for blocked same-URL POST request to finish\n" + "\n".join(log))
        wait_for_event(page_server.same_url_post_result_document_ran, "blocked same-URL POST result document")
        expect_url(webdriver_port, session_id, "after blocked same-URL POST submit", url_post_same_url_blocked, log)
        expect_body_text(webdriver_port, session_id, "after blocked same-URL POST submit", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after blocked same-URL POST submit", "post", log)
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"POST crash recovery initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_post_form)
        expect_url(webdriver_port, session_id, "after POST form load", url_post_form, log)
        post_form_submit_point = execute_script(
            webdriver_port,
            session_id,
            """
const button = document.getElementById('submit');
const rect = button.getBoundingClientRect();
return [Math.round(rect.left + rect.width / 2), Math.round(rect.top + rect.height / 2)];
""",
        )
        page_server.post_result_document_ran.clear()
        perform_pointer_click(webdriver_port, session_id, post_form_submit_point[0], post_form_submit_point[1], log)
        wait_for_event(page_server.post_result_document_ran, "POST result document")
        expect_url(webdriver_port, session_id, "after POST form submit", url_post_result, log)
        expect_body_text(webdriver_port, session_id, "after POST form submit", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after POST form submit", "post", log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after POST form submit",
            [url_post_form, url_post_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        refresh(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after POST reload", url_post_result, log)
        expect_body_text(webdriver_port, session_id, "after POST reload", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after POST reload", "post", log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after POST reload",
            [url_post_form, url_post_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after POST crash recovery", url_post_result, log)
        expect_body_text(webdriver_port, session_id, "after POST crash recovery", "POST:name=ladybird", log)
        expect_current_entry_resource(webdriver_port, session_id, "after POST crash recovery", "post", log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after POST crash recovery",
            [url_post_form, url_post_result],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )
        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = None

        session_id = create_session(webdriver_port)
        log = [f"initial: {current_url(webdriver_port, session_id)}"]
        load_url_from_ui(webdriver_port, session_id, url_a)
        expect_url(webdriver_port, session_id, "after /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after /a",
            [url_a],
            [0],
            0,
            False,
            False,
            log,
            expected_web_content_known_used_steps=[0],
            expected_web_content_current_step=0,
        )

        load_url_from_ui(webdriver_port, session_id, url_redirect_to_b)
        expect_url(webdriver_port, session_id, "after UI redirect navigation to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI redirect navigation to /b",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after back from UI redirect navigation", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after back from UI redirect navigation",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )

        navigate_from_renderer_using_link(
            webdriver_port,
            session_id,
            "#redirect",
            url_b,
            log,
            page_server.b_document_ran,
            "B document after renderer redirect link",
            expected_scheduled_url=url_redirect_to_b,
        )
        expect_url(webdriver_port, session_id, "after renderer redirect navigation to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after renderer redirect navigation to /b",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after back from renderer redirect navigation", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after back from renderer redirect navigation",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after forward from renderer redirect navigation", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after forward from renderer redirect navigation",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after returning to /a from renderer redirect navigation", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after returning to /a from renderer redirect navigation",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )

        navigate_from_renderer_using_link(
            webdriver_port, session_id, "#go", url_b, log, page_server.b_document_ran, "B document after link"
        )
        expect_url(webdriver_port, session_id, "after renderer navigation to /b", url_b, log)
        after_renderer_navigation_to_b = expect_ui_session_history(
            webdriver_port,
            session_id,
            "after renderer navigation to /b",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        page_server.a_document_ran.clear()
        execute_script(webdriver_port, session_id, "history.back(); return location.href;")
        wait_for_event(page_server.a_document_ran, "A document after page-initiated history.back()")
        expect_url(webdriver_port, session_id, "after page-initiated history.back() to /a", url_a, log)
        after_page_initiated_history_back_to_a = expect_ui_session_history(
            webdriver_port,
            session_id,
            "after page-initiated history.back() to /a",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        if (
            after_page_initiated_history_back_to_a["ui"]["webContentProcessID"]
            == after_renderer_navigation_to_b["ui"]["webContentProcessID"]
        ):
            raise AssertionError(
                "Expected page-initiated cross-site history.back() to swap processes\n" + "\n".join(log)
            )

        page_server.b_document_ran.clear()
        execute_script(webdriver_port, session_id, "history.forward(); return location.href;")
        wait_for_event(page_server.b_document_ran, "B document after page-initiated history.forward()")
        expect_url(webdriver_port, session_id, "after page-initiated history.forward() to /b", url_b, log)
        after_page_initiated_history_forward_to_b = expect_ui_session_history(
            webdriver_port,
            session_id,
            "after page-initiated history.forward() to /b",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )
        if (
            after_page_initiated_history_forward_to_b["ui"]["webContentProcessID"]
            == after_page_initiated_history_back_to_a["ui"]["webContentProcessID"]
        ):
            raise AssertionError(
                "Expected page-initiated cross-site history.forward() to swap processes\n" + "\n".join(log)
            )

        expect_sandboxed_history_back_to_be_blocked(webdriver_port, session_id, url_a, url_b, log)

        before_script_initiated_blocked_back = after_page_initiated_history_forward_to_b
        perform_pointer_click(webdriver_port, session_id, 5, 5, log)
        script_beforeunload_setup = execute_script(
            webdriver_port,
            session_id,
            """
window.scriptBeforeUnloadCount = 0;
window.onbeforeunload = event => {
    ++window.scriptBeforeUnloadCount;
    event.preventDefault();
    event.returnValue = "blocked";
    return "blocked";
};
return [location.href, window.scriptBeforeUnloadCount, navigator.userActivation.hasBeenActive];
""",
        )
        if script_beforeunload_setup != [url_b, 0, True]:
            raise AssertionError(
                f"Expected script beforeunload setup on /b to be {[url_b, 0, True]}, "
                f"got {script_beforeunload_setup}\n" + "\n".join(log)
            )
        execute_script(webdriver_port, session_id, "history.back(); return location.href;")

        def script_back_canceled_by_beforeunload(result):
            return (
                isinstance(result, list)
                and len(result) == 2
                and result[0] == url_b
                and isinstance(result[1], int)
                and result[1] >= 1
            )

        script_blocked_back_state = wait_for_script_result(
            webdriver_port,
            session_id,
            "blocked script-initiated cross-site history.back() from /b",
            "return [location.href, window.scriptBeforeUnloadCount];",
            script_back_canceled_by_beforeunload,
            log,
        )
        if script_blocked_back_state != [url_b, 1]:
            raise AssertionError(
                f"Expected beforeunload to cancel script-initiated cross-site history.back(), "
                f"got {script_blocked_back_state}\n" + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked script-initiated cross-site history.back()",
            history_entry_urls(before_script_initiated_blocked_back["ui"]),
            history_used_steps(before_script_initiated_blocked_back["ui"]),
            before_script_initiated_blocked_back["ui"]["currentUsedStepIndex"],
            before_script_initiated_blocked_back["ui"]["backButtonEnabled"],
            before_script_initiated_blocked_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_script_initiated_blocked_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_script_initiated_blocked_back["ui"]["webContentCurrentStep"],
        )
        execute_script(webdriver_port, session_id, "window.onbeforeunload = null; return null;")

        page_server.a_document_ran.clear()
        perform_browser_history_shortcut(webdriver_port, session_id, "left", log)
        wait_for_event(page_server.a_document_ran, "A document after browser shortcut back")
        wait_for_ui_session_history(
            webdriver_port,
            session_id,
            "after cross-site browser shortcut back to /a converges",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )
        expect_url(webdriver_port, session_id, "after cross-site browser shortcut back to /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after cross-site browser shortcut back to /a",
            [url_a, url_b],
            [0, 1],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=0,
        )

        page_server.b_document_ran.clear()
        perform_browser_history_shortcut(webdriver_port, session_id, "right", log)
        wait_for_event(page_server.b_document_ran, "B document after browser shortcut forward")
        wait_for_ui_session_history(
            webdriver_port,
            session_id,
            "after cross-site browser shortcut forward to /b converges",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )
        expect_url(webdriver_port, session_id, "after cross-site browser shortcut forward to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after cross-site browser shortcut forward to /b",
            [url_a, url_b],
            [0, 1],
            1,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1],
            expected_web_content_current_step=1,
        )

        navigate_from_renderer_using_link(
            webdriver_port, session_id, "#go", url_c, log, page_server.c_document_ran, "C document after link"
        )
        expect_url(webdriver_port, session_id, "after renderer navigation to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after renderer navigation to /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        page_server.b_document_ran.clear()
        perform_browser_history_shortcut(webdriver_port, session_id, "left", log)
        wait_for_event(page_server.b_document_ran, "B document after browser shortcut back")
        wait_for_ui_session_history(
            webdriver_port,
            session_id,
            "after browser shortcut back to /b converges",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )
        expect_url(webdriver_port, session_id, "after browser shortcut back to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after browser shortcut back to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        page_server.c_document_ran.clear()
        perform_browser_history_shortcut(webdriver_port, session_id, "right", log)
        wait_for_event(page_server.c_document_ran, "C document after browser shortcut forward")
        wait_for_ui_session_history(
            webdriver_port,
            session_id,
            "after browser shortcut forward to /c converges",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )
        expect_url(webdriver_port, session_id, "after browser shortcut forward to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after browser shortcut forward to /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after back to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after back to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after back to /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after back to /a",
            [url_a, url_b, url_c],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after back no-op at start", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after back no-op at start",
            [url_a, url_b, url_c],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after second back no-op at start", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after second back no-op at start",
            [url_a, url_b, url_c],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after forward to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after forward to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after forward to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after forward to /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after forward no-op at end", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after forward no-op at end",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after crash recovery at /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery at /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        page_server.b_document_ran.clear()
        execute_script(webdriver_port, session_id, "history.back(); return location.href;")
        wait_for_event(page_server.b_document_ran, "B document after crash-recovered page history.back()")
        expect_url(webdriver_port, session_id, "after crash recovery page-initiated back to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery page-initiated back to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        page_server.c_document_ran.clear()
        execute_script(webdriver_port, session_id, "history.forward(); return location.href;")
        wait_for_event(page_server.c_document_ran, "C document after crash-recovered page history.forward()")
        expect_url(webdriver_port, session_id, "after crash recovery page-initiated forward to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery page-initiated forward to /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after crash recovery browser UI back to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery browser UI back to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after crash recovery back to /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery back to /a",
            [url_a, url_b, url_c],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after crash recovery forward to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery forward to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after crash recovery forward to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery forward to /c",
            [url_a, url_b, url_c],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after crash recovery branch back to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after crash recovery branch back to /b",
            [url_a, url_b, url_c],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        navigate_from_renderer_using_link(
            webdriver_port, session_id, "#branch", url_d, log, page_server.d_document_ran, "D document after link"
        )
        expect_url(webdriver_port, session_id, "after renderer branch navigation to /d", url_d, log)
        initial_history_state_after_branch = execute_script(
            webdriver_port,
            session_id,
            """
return [
    window.initialHistoryLength,
    window.initialNavigationEntryCount,
    window.initialNavigationCurrentIndex,
];
""",
        )
        # history.length counts all entries, but navigation.entries() only
        # exposes the same-origin contiguous entries around the current entry.
        expected_initial_history_state_after_branch = [3, 1, 0]
        if initial_history_state_after_branch != expected_initial_history_state_after_branch:
            raise AssertionError(
                f"Expected /d initial history state to be {expected_initial_history_state_after_branch}, "
                f"got {initial_history_state_after_branch}\n" + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after renderer branch navigation to /d",
            [url_a, url_b, url_d],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after /d crash recovery", url_d, log)
        initial_history_state_after_branch_crash_recovery = execute_script(
            webdriver_port,
            session_id,
            """
return [
    window.initialHistoryLength,
    window.initialNavigationEntryCount,
    window.initialNavigationCurrentIndex,
];
""",
        )
        if initial_history_state_after_branch_crash_recovery != expected_initial_history_state_after_branch:
            raise AssertionError(
                f"Expected /d crash recovery initial history state to be {expected_initial_history_state_after_branch}, "
                f"got {initial_history_state_after_branch_crash_recovery}\n" + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after /d crash recovery",
            [url_a, url_b, url_d],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after UI back from /d to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI back from /d to /b",
            [url_a, url_b, url_d],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after UI back from /b to /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI back from /b to /a",
            [url_a, url_b, url_d],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after UI back no-op at /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI back no-op at /a",
            [url_a, url_b, url_d],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after second UI back no-op at /a", url_a, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after second UI back no-op at /a",
            [url_a, url_b, url_d],
            [0, 1, 2],
            0,
            False,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=0,
        )

        traverse_history_from_ui(webdriver_port, session_id, 1)
        expect_url(webdriver_port, session_id, "after UI forward from /a to /b", url_b, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI forward from /a to /b",
            [url_a, url_b, url_d],
            [0, 1, 2],
            1,
            True,
            True,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=1,
        )

        traverse_history_from_ui(webdriver_port, session_id, 1)
        expect_url(webdriver_port, session_id, "after UI forward from /b to /d", url_d, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after UI forward from /b to /d",
            [url_a, url_b, url_d],
            [0, 1, 2],
            2,
            True,
            False,
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=[0, 1, 2],
            expected_web_content_current_step=2,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_state})
        expect_url(webdriver_port, session_id, "after /state", url_state, log)
        state_setup = execute_script(
            webdriver_port,
            session_id,
            """
window.name = "ladybird-history-name";
history.scrollRestoration = "manual";
history.replaceState({ classic: "replace" }, "", "/state?replace");
navigation.updateCurrentEntry({ state: { navigation: "replace" } });
const replaceKey = navigation.currentEntry.key;
const replaceId = navigation.currentEntry.id;
history.pushState({ classic: "push" }, "", "/state?push");
navigation.updateCurrentEntry({ state: { navigation: "push" } });
return [
    location.href,
    JSON.stringify(history.state),
    JSON.stringify(navigation.currentEntry.getState()),
    history.scrollRestoration,
    replaceKey,
    replaceId,
    navigation.currentEntry.key,
    navigation.currentEntry.id,
    window.name,
];
""",
        )
        expected_state_setup = [
            url_state_push,
            '{"classic":"push"}',
            '{"navigation":"push"}',
            "manual",
        ]
        if state_setup[:4] != expected_state_setup:
            raise AssertionError(
                f"Expected state setup to start with {expected_state_setup}, got {state_setup}\n" + "\n".join(log)
            )
        replace_key, replace_id, push_key, push_id, window_name = state_setup[4:]
        if window_name != "ladybird-history-name":
            raise AssertionError(f"Expected window.name setup to survive setup, got {window_name}\n" + "\n".join(log))

        def ui_history_is_at_state_push(snapshot):
            ui_history = snapshot["ui"]
            current_index = ui_history["currentUsedStepIndex"]
            if current_index < 0 or current_index >= len(ui_history["entries"]):
                return False
            return (
                history_entry_urls(ui_history)[current_index] == url_state_push
                and ui_history["webContentHistoryMatchesUI"]
            )

        wait_for_session_history(
            webdriver_port,
            session_id,
            "before state crash recovery",
            ui_history_is_at_state_push,
            log,
        )
        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after state crash recovery", url_state_push, log)
        expect_window_name(
            webdriver_port,
            session_id,
            "after state crash restores window name",
            "ladybird-history-name",
            log,
        )
        expect_history_entry_state(
            webdriver_port,
            session_id,
            "after state crash restores current entry state",
            url_state_push,
            '{"classic":"push"}',
            '{"navigation":"push"}',
            "manual",
            push_key,
            push_id,
            log,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after state crash recovery back", url_state_replace, log)
        expect_history_entry_state(
            webdriver_port,
            session_id,
            "after state crash restores previous entry state",
            url_state_replace,
            '{"classic":"replace"}',
            '{"navigation":"replace"}',
            "manual",
            replace_key,
            replace_id,
            log,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after state crash recovery forward", url_state_push, log)
        expect_history_entry_state(
            webdriver_port,
            session_id,
            "after state crash restores forward entry state",
            url_state_push,
            '{"classic":"push"}',
            '{"navigation":"push"}',
            "manual",
            push_key,
            push_id,
            log,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_scroll})
        expect_url(webdriver_port, session_id, "after /scroll", url_scroll, log)
        scroll_setup = execute_script(
            webdriver_port,
            session_id,
            """
history.scrollRestoration = "auto";
scrollTo(0, 1200);
history.replaceState({ scroll: "saved" }, "", "/scroll?saved");
return [location.href, Math.round(scrollX), Math.round(scrollY)];
""",
        )
        expected_scroll_setup = [url_scroll_saved, 0, 1200]
        if scroll_setup != expected_scroll_setup:
            raise AssertionError(
                f"Expected scroll setup to be {expected_scroll_setup}, got {scroll_setup}\n" + "\n".join(log)
            )
        expect_url(webdriver_port, session_id, "after scroll setup", url_scroll_saved, log)

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after scroll crash recovery", url_scroll_saved, log)
        expect_scroll_position(webdriver_port, session_id, "after scroll crash restores viewport", 0, 1200, log)

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_b})
        expect_url(webdriver_port, session_id, "after leaving scrolled page", url_b, log)

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after restoring scrolled page", url_scroll_saved, log)
        expect_scroll_position(webdriver_port, session_id, "after back restores viewport", 0, 1200, log)

        crash_current_page(webdriver_port, session_id)
        expect_url(webdriver_port, session_id, "after restored scroll crash recovery", url_scroll_saved, log)
        expect_scroll_position(
            webdriver_port,
            session_id,
            "after restored scroll crash recovery keeps viewport",
            0,
            1200,
            log,
        )

        request(webdriver_port, "DELETE", f"/session/{session_id}")
        session_id = create_session(webdriver_port)
        log.append(f"intercepted same-document scroll initial: {current_url(webdriver_port, session_id)}")
        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_scroll})
        expect_url(webdriver_port, session_id, "before intercepted scroll setup", url_scroll, log)

        intercepted_scroll_setup = execute_script(
            webdriver_port,
            session_id,
            """
navigation.onnavigate = null;
history.scrollRestoration = "auto";
scrollTo(0, 900);
history.replaceState({ scroll: "intercept-source" }, "", "/scroll?intercept-source");
history.pushState({ scroll: "intercept-current" }, "", "/scroll?intercept-current");
scrollTo(0, 0);
navigation.onnavigate = event => {
    if (event.navigationType === "traverse")
        event.intercept({ scroll: "manual", handler: () => Promise.resolve() });
};
return [location.href, Math.round(scrollX), Math.round(scrollY)];
""",
        )
        url_scroll_intercept_source = f"http://localhost:{page_port}/scroll?intercept-source"
        url_scroll_intercept_current = f"http://localhost:{page_port}/scroll?intercept-current"
        expected_intercepted_scroll_setup = [url_scroll_intercept_current, 0, 0]
        if intercepted_scroll_setup != expected_intercepted_scroll_setup:
            raise AssertionError(
                f"Expected intercepted scroll setup to be {expected_intercepted_scroll_setup}, got {intercepted_scroll_setup}\n"
                + "\n".join(log)
            )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        wait_for_url(webdriver_port, session_id, "after intercepted scroll back", url_scroll_intercept_source, log)
        expect_scroll_position(
            webdriver_port, session_id, "after intercepted back suppresses scroll restore", 0, 0, log
        )

        browser_ui_traverse_setup = execute_script(
            webdriver_port,
            session_id,
            """
navigation.onnavigate = null;
history.pushState({ cancel: "current" }, "", "/scroll?cancel-current");
window.canceledTraverseCount = 0;
navigation.onnavigate = event => {
    if (event.navigationType === "traverse") {
        ++window.canceledTraverseCount;
        event.preventDefault();
    }
};
return [location.href, window.canceledTraverseCount];
""",
        )
        url_scroll_cancel_current = f"http://localhost:{page_port}/scroll?cancel-current"
        expected_browser_ui_traverse_setup = [url_scroll_cancel_current, 0]
        if browser_ui_traverse_setup != expected_browser_ui_traverse_setup:
            raise AssertionError(
                f"Expected browser UI traverse setup to be {expected_browser_ui_traverse_setup}, got {browser_ui_traverse_setup}\n"
                + "\n".join(log)
            )

        def browser_ui_traverse_setup_is_mirrored(snapshot):
            ui_history = snapshot["ui"]
            if (
                not ui_history["webContentHistoryMatchesUI"]
                or ui_history["waitingToSeedWebContent"]
                or ui_history["waitingForWebContentSeedAck"]
                or ui_history["ignoringWebContentUpdatesUntilSeed"]
                or ui_history["reseedAfterCurrentHistoryLoad"]
                or ui_history["pendingWebContentHistoryStepAfterFallbackLoad"] is not None
                or ui_history["pendingSessionHistoryNavigation"] is not None
                or ui_history["pendingSessionHistoryTraversal"] is not None
                or comparable_history(ui_history) != comparable_history(snapshot["webContent"])
            ):
                return False
            return history_current_entry(ui_history)["url"] == url_scroll_cancel_current

        history_before_browser_ui_traverse = wait_for_session_history(
            webdriver_port,
            session_id,
            "before browser UI traverse with navigate cancel handler",
            browser_ui_traverse_setup_is_mirrored,
            log,
        )
        if history_before_browser_ui_traverse["ui"]["currentUsedStepIndex"] == 0:
            raise AssertionError(
                "Expected browser UI traverse setup to have a previous history entry\n" + "\n".join(log)
            )
        expected_browser_ui_traverse_url = history_entry_urls(history_before_browser_ui_traverse["ui"])[
            history_before_browser_ui_traverse["ui"]["currentUsedStepIndex"] - 1
        ]
        traverse_history_from_ui(webdriver_port, session_id, -1)
        browser_ui_traverse_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.canceledTraverseCount];",
        )
        if browser_ui_traverse_state != [expected_browser_ui_traverse_url, 1]:
            raise AssertionError(
                f"Expected browser UI traverse to ignore non-cancelable preventDefault() and move to "
                f"{[expected_browser_ui_traverse_url, 1]}, got {browser_ui_traverse_state}\n" + "\n".join(log)
            )
        history_after_browser_ui_traverse = expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "after browser UI traverse with navigate cancel handler", log
        )
        log.append(f"after browser UI traverse: {summarize_history_snapshot(history_after_browser_ui_traverse)}")

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_nested})
        expect_url(webdriver_port, session_id, "after /nested", url_nested, log)
        expect_frame_url(webdriver_port, session_id, "after initial frame load", url_frame_a, log, "Frame A")
        expect_current_entry_nested_history(webdriver_port, session_id, "after /nested", url_nested, [url_frame_a], log)

        page_server.release_blocked_frame_b.set()
        page_server.frame_b_blocked_document_ran.clear()
        execute_script(
            webdriver_port,
            session_id,
            f"document.getElementById('frame').contentWindow.location.href = '{url_frame_b_blocked}'; return null;",
        )
        wait_for_event(page_server.frame_b_blocked_document_ran, "nested frame document")
        expect_frame_url(
            webdriver_port, session_id, "after nested frame navigation", url_frame_b_blocked, log, "Frame B Blocked"
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested frame navigation",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )

        page_server.frame_b_blocked_document_ran.clear()
        crash_current_page(webdriver_port, session_id)
        wait_for_event(page_server.frame_b_blocked_document_ran, "nested frame document after crash recovery")
        expect_url(webdriver_port, session_id, "after nested crash recovery", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested crash restores iframe",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested crash recovery",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_no_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "after nested crash recovery", log
        )
        expect_web_content_session_history_matches_ui(webdriver_port, session_id, "after nested crash recovery", log)

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after nested crash recovery frame back", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested crash recovery frame back",
            url_frame_a,
            log,
            "Frame A",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested crash recovery frame back",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "after nested crash recovery frame back", log
        )

        request(webdriver_port, "POST", f"/session/{session_id}/forward", {})
        expect_url(webdriver_port, session_id, "after nested crash recovery frame forward", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after nested crash recovery frame forward",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested crash recovery frame forward",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "after nested crash recovery frame forward", log
        )

        same_document_nested_setup = execute_script(
            webdriver_port,
            session_id,
            """
history.pushState({ nested: "same-document" }, "", "/nested?same-document");
return location.href;
""",
        )
        if same_document_nested_setup != url_nested_same_document:
            raise AssertionError(
                f"Expected same-document nested setup to be {url_nested_same_document}, "
                f"got {same_document_nested_setup}\n" + "\n".join(log)
            )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after same-document nested setup",
            url_nested_same_document,
            [url_frame_a, url_frame_b_blocked],
            log,
        )

        page_server.frame_b_blocked_document_ran.clear()
        crash_current_page(webdriver_port, session_id)
        wait_for_event(
            page_server.frame_b_blocked_document_ran,
            "same-document nested frame document after crash recovery",
        )
        expect_url(
            webdriver_port, session_id, "after same-document nested crash recovery", url_nested_same_document, log
        )
        expect_frame_url(
            webdriver_port,
            session_id,
            "after same-document nested crash restores iframe",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after same-document nested crash recovery",
            url_nested_same_document,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_no_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "after same-document nested crash recovery", log
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after same-document nested back", url_nested, log)
        expect_frame_url(
            webdriver_port,
            session_id,
            "after same-document nested back keeps iframe history",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after same-document nested back",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_b})
        expect_url(webdriver_port, session_id, "after cross-site /b", url_b, log)
        expect_current_top_level_history_url(webdriver_port, session_id, "after cross-site /b", url_b, log)
        expect_navigation_buttons(webdriver_port, session_id, "after cross-site /b", True, False, log)
        expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "after cross-site /b process swap", log
        )

        before_blocked_browser_ui_back = expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "before blocked browser UI back from /b", log
        )
        navigate_event_cancel_setup = execute_script(
            webdriver_port,
            session_id,
            """
window.canceledCrossSiteNavigateCount = 0;
navigation.onnavigate = event => {
    if (event.navigationType === "push") {
        ++window.canceledCrossSiteNavigateCount;
        event.preventDefault();
    }
};
return [location.href, window.canceledCrossSiteNavigateCount];
""",
        )
        if navigate_event_cancel_setup != [url_b, 0]:
            raise AssertionError(
                f"Expected navigate event cancel setup on /b to be {[url_b, 0]}, got {navigate_event_cancel_setup}\n"
                + "\n".join(log)
            )
        cross_site_navigate_event_click_point = execute_script(
            webdriver_port,
            session_id,
            """
const rect = document.querySelector("#branch").getBoundingClientRect();
return [Math.floor(rect.left + rect.width / 2), Math.floor(rect.top + rect.height / 2)];
""",
        )
        perform_pointer_click(
            webdriver_port,
            session_id,
            cross_site_navigate_event_click_point[0],
            cross_site_navigate_event_click_point[1],
            log,
        )
        canceled_cross_site_navigate_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.canceledCrossSiteNavigateCount];",
        )
        if canceled_cross_site_navigate_state != [url_b, 1]:
            raise AssertionError(
                f"Expected navigate event to cancel cross-site link navigation from /b, got {canceled_cross_site_navigate_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after navigate event canceled cross-site link navigation from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )
        execute_script(webdriver_port, session_id, "navigation.onnavigate = null; return null;")

        expect_javascript_noop_webdriver_navigation_does_not_change_history(
            webdriver_port,
            session_id,
            url_b,
            before_blocked_browser_ui_back,
            log,
        )

        expect_javascript_noop_ui_load_does_not_change_history(
            webdriver_port,
            session_id,
            url_b,
            before_blocked_browser_ui_back,
            log,
        )

        beforeunload_setup = execute_script(
            webdriver_port,
            session_id,
            """
window.beforeUnloadCount = 0;
window.onbeforeunload = event => {
    ++window.beforeUnloadCount;
    event.preventDefault();
    event.returnValue = "blocked";
    return "blocked";
};
return [location.href, window.beforeUnloadCount];
""",
        )
        if beforeunload_setup != [url_b, 0]:
            raise AssertionError(
                f"Expected beforeunload setup on /b to be {[url_b, 0]}, got {beforeunload_setup}\n" + "\n".join(log)
            )
        link_click_point = execute_script(
            webdriver_port,
            session_id,
            """
const rect = document.querySelector("#go").getBoundingClientRect();
return [Math.floor(rect.left + rect.width / 2), Math.floor(rect.top + rect.height / 2)];
""",
        )
        perform_pointer_click(webdriver_port, session_id, link_click_point[0], link_click_point[1], log)
        user_activation = execute_script(webdriver_port, session_id, "return navigator.userActivation.hasBeenActive;")
        if user_activation is not True:
            raise AssertionError("Expected pointer click to give /b sticky activation\n" + "\n".join(log))

        blocked_link_navigation_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if blocked_link_navigation_state[0] != url_b or blocked_link_navigation_state[1] < 1:
            raise AssertionError(
                f"Expected beforeunload to cancel link navigation from /b, got {blocked_link_navigation_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked link navigation from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )

        blocked_stale_ui_load_state = expect_beforeunload_cancels_stale_ui_load(
            webdriver_port,
            session_id,
            url_b,
            url_c,
            before_blocked_browser_ui_back,
            blocked_link_navigation_state,
            log,
        )

        blocked_webdriver_navigate_state = expect_beforeunload_cancels_webdriver_navigation(
            webdriver_port,
            session_id,
            url_c,
            "WebDriver navigation from /b",
            url_b,
            blocked_stale_ui_load_state[1],
            before_blocked_browser_ui_back,
            log,
        )

        blocked_cross_site_webdriver_navigate_state = expect_beforeunload_cancels_webdriver_navigation(
            webdriver_port,
            session_id,
            url_d,
            "cross-site WebDriver navigation from /b",
            url_b,
            blocked_webdriver_navigate_state[1],
            before_blocked_browser_ui_back,
            log,
        )

        blocked_refresh_state = expect_beforeunload_cancels_refresh(
            webdriver_port,
            session_id,
            url_b,
            blocked_cross_site_webdriver_navigate_state[1],
            log,
        )

        traverse_history_from_ui(webdriver_port, session_id, -1)
        blocked_beforeunload_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if blocked_beforeunload_state[0] != url_b or blocked_beforeunload_state[1] <= blocked_refresh_state[1]:
            raise AssertionError(
                f"Expected beforeunload to cancel browser UI back from /b, got {blocked_beforeunload_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked browser UI back from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        blocked_webdriver_back_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if blocked_webdriver_back_state[0] != url_b or blocked_webdriver_back_state[1] <= blocked_beforeunload_state[1]:
            raise AssertionError(
                f"Expected beforeunload to cancel WebDriver back from /b, got {blocked_webdriver_back_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked WebDriver back from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )

        perform_browser_history_shortcut(webdriver_port, session_id, "left", log)
        blocked_shortcut_back_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if blocked_shortcut_back_state[0] != url_b or blocked_shortcut_back_state[1] <= blocked_webdriver_back_state[1]:
            raise AssertionError(
                f"Expected beforeunload to cancel browser history shortcut from /b, got {blocked_shortcut_back_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked browser history shortcut from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )

        cross_site_link_click_point = execute_script(
            webdriver_port,
            session_id,
            """
const rect = document.querySelector("#branch").getBoundingClientRect();
return [Math.floor(rect.left + rect.width / 2), Math.floor(rect.top + rect.height / 2)];
""",
        )
        perform_pointer_click(
            webdriver_port, session_id, cross_site_link_click_point[0], cross_site_link_click_point[1], log
        )
        blocked_cross_site_link_navigation_state = execute_script(
            webdriver_port,
            session_id,
            "return [location.href, window.beforeUnloadCount];",
        )
        if (
            blocked_cross_site_link_navigation_state[0] != url_b
            or blocked_cross_site_link_navigation_state[1] <= blocked_shortcut_back_state[1]
        ):
            raise AssertionError(
                f"Expected beforeunload to cancel cross-site link navigation from /b, got {blocked_cross_site_link_navigation_state}\n"
                + "\n".join(log)
            )
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after blocked cross-site link navigation from /b",
            history_entry_urls(before_blocked_browser_ui_back["ui"]),
            history_used_steps(before_blocked_browser_ui_back["ui"]),
            before_blocked_browser_ui_back["ui"]["currentUsedStepIndex"],
            before_blocked_browser_ui_back["ui"]["backButtonEnabled"],
            before_blocked_browser_ui_back["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_blocked_browser_ui_back["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_blocked_browser_ui_back["ui"]["webContentCurrentStep"],
        )
        execute_script(webdriver_port, session_id, "window.onbeforeunload = null; return null;")

        page_server.release_blocked_frame_b.clear()
        page_server.blocked_frame_b_requested.clear()
        traverse_history_from_ui(webdriver_port, session_id, -1)
        expect_url(webdriver_port, session_id, "after browser UI back to nested", url_nested, log)
        expect_navigation_buttons(webdriver_port, session_id, "after browser UI back to nested", True, True, log)
        wait_for_event(page_server.blocked_frame_b_requested, "blocked nested frame restore")
        expect_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "while browser UI back restores nested frame navigation", log
        )
        page_server.frame_b_blocked_document_ran.clear()
        page_server.release_blocked_frame_b.set()
        wait_for_event(
            page_server.frame_b_blocked_document_ran,
            "nested frame document after browser UI back",
        )
        expect_frame_url(
            webdriver_port,
            session_id,
            "after browser UI back restores nested frame navigation",
            url_frame_b_blocked,
            log,
            "Frame B Blocked",
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after browser UI back to nested",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )
        expect_no_pending_web_content_history_step_after_fallback_load(
            webdriver_port, session_id, "after browser UI back restores nested frame navigation", log
        )

        request(webdriver_port, "POST", f"/session/{session_id}/back", {})
        expect_url(webdriver_port, session_id, "after nested frame back", url_nested, log)
        expect_navigation_buttons(webdriver_port, session_id, "after nested frame back", True, True, log)
        expect_frame_url(
            webdriver_port, session_id, "after nested frame back restores previous entry", url_frame_a, log, "Frame A"
        )
        expect_current_entry_nested_history(
            webdriver_port,
            session_id,
            "after nested frame back",
            url_nested,
            [url_frame_a, url_frame_b_blocked],
            log,
        )

        request(webdriver_port, "POST", f"/session/{session_id}/url", {"url": url_b})
        expect_url(webdriver_port, session_id, "after branching from nested frame history", url_b, log)
        expect_entry_nested_history(
            webdriver_port,
            session_id,
            "after branching from nested frame history",
            url_nested,
            [url_frame_a],
            log,
        )

        before_location_replace = expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "before location.replace()", log
        )
        replace_current_index = before_location_replace["ui"]["currentUsedStepIndex"]
        expected_urls_after_replace = history_entry_urls(before_location_replace["ui"])
        expected_urls_after_replace[replace_current_index] = url_c
        page_server.c_document_ran.clear()
        execute_script(webdriver_port, session_id, f"location.replace({json.dumps(url_c)}); return null;")
        wait_for_event(page_server.c_document_ran, "C document script after location.replace()")
        expect_url(webdriver_port, session_id, "after location.replace() to /c", url_c, log)
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "after location.replace() to /c",
            expected_urls_after_replace,
            history_used_steps(before_location_replace["ui"]),
            replace_current_index,
            before_location_replace["ui"]["backButtonEnabled"],
            before_location_replace["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=True,
            expected_web_content_known_used_steps=history_step_values(
                before_location_replace["ui"]["webContentKnownUsedSteps"]
            ),
            expected_web_content_current_step=before_location_replace["ui"]["webContentCurrentStep"],
        )

        load_url_from_ui(webdriver_port, session_id, url_reload_blocked)
        expect_url(webdriver_port, session_id, "after /reload-blocked", url_reload_blocked, log)
        expect_current_ui_entry_reload_pending(webdriver_port, session_id, "before blocked reload", False, log)
        page_server.blocked_reload_requested.clear()
        page_server.reload_recovery_requested.clear()
        page_server.release_blocked_reload.clear()
        page_server.reload_blocked_document_ran.clear()
        refresh(webdriver_port, session_id)
        expect_reload_pending_history_log = True
        wait_for_event(page_server.blocked_reload_requested, "blocked reload")
        expect_current_ui_entry_reload_pending(webdriver_port, session_id, "during blocked reload", True, log)
        page_server.release_blocked_reload.set()
        wait_for_event(page_server.reload_blocked_document_ran, "blocked reload document")
        expect_url(webdriver_port, session_id, "after blocked reload completes", url_reload_blocked, log)
        expect_current_ui_entry_reload_pending(webdriver_port, session_id, "after blocked reload completes", False, log)
        before_blocked_reload_crash_recovery = expect_web_content_session_history_matches_ui(
            webdriver_port, session_id, "before blocked reload crash recovery", log
        )

        page_server.blocked_reload_requested.clear()
        page_server.reload_recovery_requested.clear()
        page_server.release_blocked_reload.clear()
        page_server.reload_blocked_document_ran.clear()
        refresh(webdriver_port, session_id)
        wait_for_event(page_server.blocked_reload_requested, "blocked reload before crash")
        expect_current_ui_entry_reload_pending(
            webdriver_port, session_id, "during blocked reload before crash", True, log
        )
        crash_current_page_allowing_navigation_timeout(webdriver_port, session_id)
        wait_for_event(page_server.reload_recovery_requested, "reload recovery request")
        expect_ui_session_history(
            webdriver_port,
            session_id,
            "while blocked crash recovery reloads current entry",
            history_entry_urls(before_blocked_reload_crash_recovery["ui"]),
            history_used_steps(before_blocked_reload_crash_recovery["ui"]),
            before_blocked_reload_crash_recovery["ui"]["currentUsedStepIndex"],
            before_blocked_reload_crash_recovery["ui"]["backButtonEnabled"],
            before_blocked_reload_crash_recovery["ui"]["forwardButtonEnabled"],
            log,
            expect_web_content_matches_ui=False,
            expected_waiting_to_seed_web_content=True,
            expected_waiting_for_web_content_seed_ack=False,
            expected_ignoring_web_content_updates_until_seed=True,
            expected_reseed_after_current_history_load=True,
        )
        page_server.release_blocked_reload.set()
        wait_for_event(page_server.reload_blocked_document_ran, "reload document after crash recovery")
        expect_url(webdriver_port, session_id, "after blocked reload crash recovery", url_reload_blocked, log)
        expect_current_ui_entry_reload_pending(
            webdriver_port, session_id, "after blocked reload crash recovery", False, log
        )

        if baseline_open_fds is not None:
            open_fds = count_open_fds(webdriver.pid)
            if open_fds is not None and open_fds - baseline_open_fds > 64:
                raise AssertionError(
                    f"WebDriver leaked file descriptors: {baseline_open_fds} before the test, {open_fds} after"
                )
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

        page_server.release_blocked_frame_b.set()
        page_server.release_blocked_reload.set()
        page_server.release_blocked_process_swap_back.set()
        page_server.release_blocked_forward.set()
        page_server.shutdown()
        page_server.server_close()

        if not failed and expect_reload_pending_history_log and "reload_pending=true" not in stderr:
            print(stdout, file=sys.stdout)
            print(stderr, file=sys.stderr)
            raise AssertionError("Expected session history debug log to include reload_pending=true")

        if not failed and expect_no_entry_history_log and "reason=traverse-no-entry" not in stderr:
            print(stdout, file=sys.stdout)
            print(stderr, file=sys.stderr)
            raise AssertionError("Expected session history debug log to include reason=traverse-no-entry")

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
