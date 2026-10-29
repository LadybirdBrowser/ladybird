#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import base64
import hashlib
import http.client
import http.server
import importlib
import json
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse

helpers = importlib.import_module("test-webdriver-error-keep-alive")


class BiDiConnection:
    def __init__(self, url, *, host=None, origin=None, expected_status=101):
        url = urllib.parse.urlsplit(url)
        hostname, port = url.hostname, url.port
        if hostname is None or port is None:
            raise ValueError("The WebSocket URL must contain a hostname and port")
        self.socket = socket.create_connection((hostname, port), timeout=30)
        self.reader = self.socket.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        host = host or f"{hostname}:{port}"
        origin_header = f"Origin: {origin}\r\n" if origin else ""
        self.socket.sendall(
            (
                f"GET {url.path} HTTP/1.1\r\nHost: {host}\r\n"
                f"Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n{origin_header}\r\n"
            ).encode()
        )
        assert int(self.reader.readline().split()[1]) == expected_status
        headers = {}
        while (line := self.reader.readline()) != b"\r\n":
            name, value = line.decode().split(":", 1)
            headers[name.lower()] = value.strip()
        if expected_status != 101:
            return
        expected_accept = base64.b64encode(
            hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
        )
        assert headers["sec-websocket-accept"].encode() == expected_accept
        self.next_id = 0
        self.responses = {}
        self.events = []

    def close(self):
        try:
            self.reader.close()
        finally:
            self.socket.close()

    def send(self, method, params):
        self.next_id += 1
        payload = json.dumps({"id": self.next_id, "method": method, "params": params}).encode()
        mask = os.urandom(4)
        if len(payload) < 126:
            header = bytes([0x81, 0x80 | len(payload)])
        else:
            header = bytes([0x81, 0x80 | 126]) + struct.pack("!H", len(payload))
        self.socket.sendall(header + mask + bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload)))
        return self.next_id

    def read_message(self):
        first, second = self.reader.read(2)
        assert first == 0x81, f"Unexpected WebSocket frame: {first:#x}"
        assert not second & 0x80
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.reader.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.reader.read(8))[0]
        message = json.loads(self.reader.read(length))
        if "id" in message:
            self.responses[message["id"]] = message
        else:
            self.events.append(message)

    def response(self, command_id):
        while command_id not in self.responses:
            self.read_message()
        return self.responses.pop(command_id)

    def event(self, method):
        while True:
            for index, event in enumerate(self.events):
                if event["method"] == method:
                    return self.events.pop(index)["params"]
            self.read_message()

    def command(self, method, params):
        response = self.response(self.send(method, params))
        assert response["type"] == "success", response
        return response["result"]

    def evaluate(self, context, expression, **options):
        return self.command(
            "script.evaluate",
            {"target": {"context": context}, "expression": expression, "awaitPromise": False, **options},
        )


class ReloadServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), ReloadHandler)
        self.block_reload: bool = False
        self.request_received: threading.Event = threading.Event()
        self.release_response: threading.Event = threading.Event()
        self.download_headers_sent: threading.Event = threading.Event()
        self.release_download: threading.Event = threading.Event()
        self.download_body: bytes = b"BiDi navigation download\n" * 128
        self.segmented_download_body: bytes = b""
        self.range_request_received: threading.Event = threading.Event()
        self.release_initial_download: threading.Event = threading.Event()
        self.initial_download_stopped: threading.Event = threading.Event()
        self.interception_lock = threading.Lock()
        self.interception_requests = []
        self.interception_received = threading.Event()
        self.release_interception = threading.Event()
        self.detached_final_received = threading.Event()
        self.interception_transport_closed = threading.Event()
        self.document_headers_sent: threading.Event = threading.Event()
        self.release_document: threading.Event = threading.Event()
        self.preflight_received: threading.Event = threading.Event()
        self.release_preflight: threading.Event = threading.Event()
        self.document_body: bytes = (
            b"<!doctype html><title>Adopted document</title><p>" + b"BiDi adopted document\n" * 128
        )


class ReloadHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.do_GET()

    def do_PUT(self):
        self.do_GET()

    def do_GET(self):
        server = self.server
        assert isinstance(server, ReloadServer)
        if self.path == "/network-interception/fail-body":
            self.send_response(200)
            self.send_header("Content-Length", "1000000")
            self.end_headers()
            self.wfile.write(b"unfinished body")
            self.wfile.flush()
            self.connection.settimeout(30)
            try:
                if not self.connection.recv(1):
                    server.interception_transport_closed.set()
            except (ConnectionResetError, BrokenPipeError):
                server.interception_transport_closed.set()
            return
        if self.path.startswith("/network-interception/"):
            # NB: Preserve duplicate fields for assertions even if the transport combines their length values.
            # A stale length on the redirected GET is an assertion failure, not a reason for the fixture to wait.
            length = int(self.headers.get("Content-Length", "0").split(",", 1)[0])
            if self.command == "GET" and self.path.startswith("/network-interception/echo-final/"):
                length = 0
            body = self.rfile.read(length)
            record = {
                "path": self.path,
                "method": self.command,
                "body": body.decode(),
                "headers": list(self.headers.items()),
            }
            with server.interception_lock:
                server.interception_requests.append(record)
            if self.path == "/network-interception/detached-final":
                server.detached_final_received.set()
            if self.path in ["/network-interception/late-enable", "/network-interception/detached-redirect"]:
                server.interception_received.set()
                if not server.release_interception.wait(30):
                    raise TimeoutError("The test did not release its intercepted response")
            if self.path.startswith("/network-interception/echo-redirect/"):
                self.send_response(303 if self.path.endswith("303") else 307)
                self.send_header("Location", self.path.replace("/echo-redirect/", "/echo-final/", 1))
                body = b""
            elif self.path.startswith("/network-interception/redirect/"):
                target = self.path.replace("/redirect/", "/final/", 1)
                self.send_response(307)
                self.send_header("Location", target)
                body = b""
            elif self.path == "/network-interception/detached-redirect":
                self.send_response(302)
                self.send_header("Location", "/network-interception/detached-final")
                body = b""
            elif self.path.startswith("/network-interception/auth/") and self.headers.get("Authorization") is None:
                self.send_response(401)
                self.send_header("WWW-Authenticate", 'Basic realm="bidi-selective-overrides"')
                if origin := self.headers.get("Origin"):
                    self.send_header("Access-Control-Allow-Origin", origin)
                    self.send_header("Access-Control-Allow-Credentials", "true")
                body = b"authentication required"
            else:
                self.send_response(200)
                if self.path.startswith("/network-interception/cors/"):
                    self.send_header("Access-Control-Allow-Origin", "*")
                with server.interception_lock:
                    body = json.dumps(server.interception_requests).encode()
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path == "/network-download/split":
            body = server.segmented_download_body
            range_header = self.headers.get("Range")
            start, end = 0, len(body) - 1
            if range_header is not None:
                assert range_header.startswith("bytes="), range_header
                start_text, end_text = range_header.removeprefix("bytes=").split("-", 1)
                start, end = int(start_text), min(int(end_text), len(body) - 1)
            else:
                # Keep the original transfer from buffering the whole body before its segment is stopped.
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 16384)
            self.send_response(206 if range_header is not None else 200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", 'attachment; filename="bidi-split.bin"')
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("ETag", '"bidi-split"')
            if range_header is not None:
                self.send_header("Content-Range", f"bytes {start}-{end}/{len(body)}")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            self.wfile.write(body[start : start + 1024])
            self.wfile.flush()
            if range_header is not None:
                server.range_request_received.set()
                release = server.release_download
            else:
                server.download_headers_sent.set()
                release = server.release_initial_download
            try:
                if not release.wait(30):
                    raise TimeoutError("The test did not release the segmented download response")
                self.wfile.write(body[start + 1024 : end + 1])
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                if range_header is None:
                    server.initial_download_stopped.set()
            return
        if self.path.startswith("/network-download/"):
            kind = self.path.rsplit("/", 1)[-1]
            truncated = kind == "truncated"
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Disposition", f'attachment; filename="bidi-{kind}.bin"')
            self.send_header("Content-Length", str(len(server.download_body) + (16 if truncated else 0)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            try:
                self.wfile.write(server.download_body[:1024])
                self.wfile.flush()
                server.download_headers_sent.set()
                if not server.release_download.wait(30):
                    raise TimeoutError("The test did not release the download response")
                if not truncated:
                    self.wfile.write(server.download_body[1024:])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if self.path.startswith("/network-document/"):
            kind = self.path.rsplit("/", 1)[-1]
            truncated = kind == "truncated"
            body = server.document_body
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body) + (16 if truncated else 0)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            # A navigation reads 1445 bytes before it commits its document, so the held part starts after them.
            held_from = 2048
            try:
                self.wfile.write(body[:held_from])
                self.wfile.flush()
                server.document_headers_sent.set()
                if kind in ["intercepted-fail", "intercepted-provide"]:
                    # A failed or replaced response's transport closes while the rest of its body is held.
                    self.connection.settimeout(30)
                    try:
                        if not self.connection.recv(1):
                            server.interception_transport_closed.set()
                    except ConnectionResetError:
                        server.interception_transport_closed.set()
                    return
                if not server.release_document.wait(30):
                    raise TimeoutError("The test did not release the document response")
                if not truncated:
                    self.wfile.write(body[held_from:])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        if server.block_reload:
            server.request_received.set()
            if not server.release_response.wait(30):
                raise TimeoutError("The test did not release the reload response")
        if self.path == "/input-parent":
            body = (
                '<!doctype html><title>Input parent</title><input id="target">'
                f'<iframe id="input-child" src="http://localhost:{server.server_port}/input-child"></iframe>'
            ).encode()
        elif self.path == "/input-child":
            body = (
                '<!doctype html><title>Input child</title><input id="target">'
                f'<a id="navigate-top" target="_top" href="http://localhost:{server.server_port}/input-child">Navigate top</a>'
            ).encode()
        else:
            body = b"<!doctype html><title>Reload target</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def do_OPTIONS(self):
        server = self.server
        assert isinstance(server, ReloadServer)
        server.preflight_received.set()
        if not server.release_preflight.wait(30):
            raise TimeoutError("The test did not release the preflight response")
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "PUT")
        self.send_header("Access-Control-Allow-Headers", "X-BiDi")
        self.end_headers()

    def log_message(self, format, *args):
        pass


def intercepted_event(bidi, method, url):
    while True:
        event = bidi.event(method)
        if event["request"]["url"] == url:
            assert event["isBlocked"], event
            return event
        assert not event["isBlocked"], event


def take_blocked_requests(bidi, context, url):
    # An object element can fetch its data once for each time it determines what it represents.
    bidi.evaluate(context, "0")
    events = [
        event
        for event in bidi.events
        if event["method"] == "network.beforeRequestSent" and event["params"]["request"]["url"] == url
    ]
    for event in events:
        bidi.events.remove(event)
    return [event["params"]["request"]["request"] for event in events if event["params"]["isBlocked"]]


def send_fetch(bidi, context, url, options=None):
    return bidi.send(
        "script.evaluate",
        {
            "target": {"context": context},
            "expression": f"fetch({json.dumps(url)}, {json.dumps(options or {})}).then(response => response.text())",
            "awaitPromise": True,
        },
    )


def fetch_records(bidi, command):
    result = bidi.response(command)
    assert result["type"] == "success" and result["result"]["type"] == "success", result
    return json.loads(result["result"]["result"]["value"])


def check_network_late_interception(bidi, context, server, abort=False):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/late-enable"
    server.interception_received.clear()
    server.release_interception.clear()
    with server.interception_lock:
        server.interception_requests.clear()
    if abort:
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "expression": f"window.interceptionAbort = new AbortController(); fetch({json.dumps(url)}, "
                "{signal: interceptionAbort.signal}).then(() => 'fulfilled', error => error.name)",
                "awaitPromise": True,
            },
        )
    else:
        pending = send_fetch(bidi, context, url)
    assert server.interception_received.wait(30), "The unobserved request did not reach the server"
    subscription = bidi.command("session.subscribe", {"events": ["network.responseStarted"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["responseStarted"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        # NB: State broadcasts and response delivery come from different IPC peers.
        bidi.evaluate(context, "0")
        server.release_interception.set()
        event = intercepted_event(bidi, "network.responseStarted", url)
        request_id = event["request"]["request"]
        if abort:
            bidi.evaluate(context, "interceptionAbort.abort(); 0")
            result = bidi.response(pending)
            assert result["type"] == "success" and result["result"]["result"]["value"] == "AbortError", result
            result = bidi.response(bidi.send("network.continueResponse", {"request": request_id}))
            assert result["type"] == "error" and result["error"] == "no such request", result
        else:
            # A known blocked request stays resumable when its event subscription is removed.
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            subscription = None
            bidi.command("network.continueResponse", {"request": request_id})
            records = fetch_records(bidi, pending)
            assert [record["path"] for record in records] == ["/network-interception/late-enable"], records
    finally:
        server.release_interception.set()
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            if subscription is not None:
                bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_override_retries(bidi, context, server, kinds):
    origin = f"http://127.0.0.1:{server.server_port}"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent", "network.authRequired"]})[
        "subscription"
    ]

    def header_values(record, name):
        return [value for key, value in record["headers"] if key.lower() == name.lower()]

    def check_record(record, method, body, *, generated=True):
        assert record["method"] == method and record["body"] == body, record
        assert header_values(record, "Content-Length") == [str(len(body.encode()))], record
        for name in [
            "Referer",
            "Origin",
            "Sec-Fetch-Dest",
            "Sec-Fetch-Mode",
            "Sec-Fetch-Site",
            "Sec-Fetch-User",
            "User-Agent",
            "Content-Type",
            "Cache-Control",
            "Pragma",
        ]:
            assert len(header_values(record, name)) <= 1, (name, record)
        if generated:
            for name, value in [
                ("Referer", f"{origin}/interception-page"),
                ("Sec-Fetch-Dest", "empty"),
                ("Sec-Fetch-Mode", "cors"),
                ("Sec-Fetch-Site", "same-origin"),
            ]:
                assert header_values(record, name) == [value], (name, record)
            assert len(header_values(record, "User-Agent")) == 1, record
            if header_values(record, "Origin"):
                assert header_values(record, "Origin") == [origin], record

    try:
        for kind in kinds:
            with server.interception_lock:
                server.interception_requests.clear()
            start_url = f"{origin}/network-interception/start/{kind}"
            target_url = f"{origin}/network-interception/{'auth' if kind == 'auth' else 'redirect'}/{kind}"
            intercepts = [
                bidi.command(
                    "network.addIntercept",
                    {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": start_url}]},
                )["intercept"]
            ]
            if kind == "auth":
                intercepts.append(
                    bidi.command(
                        "network.addIntercept",
                        {"phases": ["authRequired"], "urlPatterns": [{"type": "string", "pattern": target_url}]},
                    )["intercept"]
                )
            pending = send_fetch(
                bidi, context, start_url, {"method": "POST", "body": "original-body", "cache": "no-store"}
            )
            event = intercepted_event(bidi, "network.beforeRequestSent", start_url)
            overrides = {"request": event["request"]["request"], "url": target_url}
            method, body = "POST", "original-body"
            if kind == "overrides":
                method, body = "PUT", "overridden payload"
                overrides.update(
                    {
                        "method": method,
                        "body": {"type": "string", "value": body},
                        "headers": [
                            {
                                "name": "Content-Type",
                                "value": {"type": "string", "value": "application/x-bidi-override"},
                            },
                            {"name": "X-Client-Override", "value": {"type": "string", "value": "kept"}},
                        ],
                        "cookies": [{"name": "bidi_override", "value": {"type": "string", "value": "kept"}}],
                    }
                )
            bidi.command("network.continueRequest", overrides)
            if kind == "auth":
                event = intercepted_event(bidi, "network.authRequired", target_url)
                bidi.command(
                    "network.continueWithAuth",
                    {
                        "request": event["request"]["request"],
                        "action": "provideCredentials",
                        "credentials": {"type": "password", "username": "user", "password": "pass"},
                    },
                )
            records = fetch_records(bidi, pending)
            assert len(records) == 2, records
            assert records[0]["path"] == urllib.parse.urlsplit(target_url).path, records
            expected_final = target_url if kind == "auth" else target_url.replace("/redirect/", "/final/", 1)
            assert records[-1]["path"] == urllib.parse.urlsplit(expected_final).path, records
            for index, record in enumerate(records):
                check_record(record, method, body, generated=kind != "overrides" or index != 0)
                if kind == "overrides":
                    assert header_values(record, "X-Client-Override") == ["kept"], record
                    assert header_values(record, "Content-Type") == ["application/x-bidi-override"], record
                    assert header_values(record, "Cookie") == ["bidi_override=kept"], record
            if kind == "auth":
                assert header_values(records[-1], "Authorization") == ["Basic dXNlcjpwYXNz"], records
            for intercept in intercepts:
                bidi.command("network.removeIntercept", {"intercept": intercept})
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_echoed_headers(bidi, context, server, kinds):
    origin = f"http://127.0.0.1:{server.server_port}"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent", "network.authRequired"]})[
        "subscription"
    ]

    subscribed = True

    def values(record, name):
        return [value for key, value in record["headers"] if key.lower() == name.lower()]

    try:
        for kind in kinds:
            if not subscribed:
                subscription = bidi.command(
                    "session.subscribe", {"events": ["network.beforeRequestSent", "network.authRequired"]}
                )["subscription"]
                subscribed = True
            with server.interception_lock:
                server.interception_requests.clear()
            start_url = f"{origin}/network-interception/start/{kind}"
            auth = kind.endswith("auth")
            target_url = f"{origin}/network-interception/{'auth' if auth else 'echo-redirect'}/{kind}"
            intercepts = [
                bidi.command(
                    "network.addIntercept",
                    {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": start_url}]},
                )["intercept"]
            ]
            if auth:
                intercepts.append(
                    bidi.command(
                        "network.addIntercept",
                        {"phases": ["authRequired"], "urlPatterns": [{"type": "string", "pattern": target_url}]},
                    )["intercept"]
                )
            options: dict[str, object] = {"method": "POST", "body": "original-body"}
            if kind.endswith("range"):
                options["headers"] = {"Range": "bytes=0-9"}
            pending = send_fetch(bidi, context, start_url, options)
            event = intercepted_event(bidi, "network.beforeRequestSent", start_url)
            headers = list(event["request"]["headers"])
            expected = {
                "Referer": f"{origin}/interception-page",
                "Origin": origin,
                "Sec-Fetch-Dest": "empty",
                "Sec-Fetch-Mode": "cors",
                "Sec-Fetch-Site": "same-origin",
                "X-Client-Override": "kept",
            }
            if kind.endswith("range"):
                expected["Accept-Encoding"] = "identity"
            changed = {"X-Client-Override": "kept"}
            if kind.startswith("explicit"):
                changed.update(
                    {
                        "Referer": f"{origin}/explicit-referrer",
                        "Origin": "null",
                        "Sec-Fetch-Dest": "object",
                        "Sec-Fetch-Mode": "no-cors",
                        "Sec-Fetch-Site": "same-site",
                    }
                )
            if kind == "explicit-range":
                changed["Accept-Encoding"] = "gzip"
            for name, value in changed.items():
                headers = [header for header in headers if header["name"].lower() != name.lower()]
                headers.append({"name": name, "value": {"type": "string", "value": value}})
            expected.update(changed)
            overrides = {"request": event["request"]["request"], "url": target_url, "headers": headers}
            body = "original-body"
            if kind == "echo-body":
                body = "replacement-body-is-longer"
                overrides["body"] = {"type": "string", "value": body}
            if kind == "explicit-unsubscribe":
                for intercept in intercepts:
                    bidi.command("network.removeIntercept", {"intercept": intercept})
                intercepts.clear()
                bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
                subscribed = False
            bidi.command("network.continueRequest", overrides)
            if auth:
                event = intercepted_event(bidi, "network.authRequired", target_url)
                bidi.command(
                    "network.continueWithAuth",
                    {
                        "request": event["request"]["request"],
                        "action": "provideCredentials",
                        "credentials": {"type": "password", "username": "user", "password": "pass"},
                    },
                )
            records = fetch_records(bidi, pending)
            assert len(records) == 2, records
            for index, record in enumerate(records):
                drops_body = index == 1 and kind.endswith("303")
                assert record["method"] == ("GET" if drops_body else "POST"), record
                assert record["body"] == ("" if drops_body else body), record
                assert values(record, "Content-Length") == ([] if drops_body else [str(len(body))]), record
                for name, value in expected.items():
                    assert values(record, name) == [value], (name, record)
                assert len(values(record, "Content-Type")) == (0 if drops_body else 1), record
            assert records[0]["path"] == urllib.parse.urlsplit(target_url).path, records
            final_url = target_url if auth else target_url.replace("/echo-redirect/", "/echo-final/", 1)
            assert records[-1]["path"] == urllib.parse.urlsplit(final_url).path, records
            if auth:
                assert values(records[0], "Authorization") == [], records
                assert values(records[1], "Authorization") == ["Basic dXNlcjpwYXNz"], records
            for intercept in intercepts:
                bidi.command("network.removeIntercept", {"intercept": intercept})
    finally:
        if sys.exc_info()[0] is None and subscribed:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_detached_interception(bidi, context, server):
    origin = f"http://127.0.0.1:{server.server_port}"
    url = f"{origin}/network-interception/detached-redirect"
    final_url = f"{origin}/network-interception/detached-final"
    server.interception_received.clear()
    server.release_interception.clear()
    server.detached_final_received.clear()
    with server.interception_lock:
        server.interception_requests.clear()
    bidi.evaluate(
        context,
        f"""new Promise(resolve => {{
        const frame = document.createElement('iframe');
        frame.id = 'interception-child';
        frame.onload = () => resolve(0);
        frame.src = {json.dumps(origin + "/interception-frame")};
        document.body.append(frame);
    }})""",
        awaitPromise=True,
    )
    tree = bidi.command("browsingContext.getTree", {"root": context})["contexts"][0]
    assert len(tree["children"]) == 1, tree
    assert tree["children"][0]["url"] == f"{origin}/interception-frame", tree
    child = tree["children"][0]["context"]
    bidi.evaluate(context, "window.detachedInterceptionMarker = true; 0")
    subscription = bidi.command(
        "session.subscribe",
        {
            "events": ["browsingContext.contextDestroyed", "network.beforeRequestSent", "network.responseStarted"],
        },
    )["subscription"]
    intercepts = []
    try:
        for phase, target in [("responseStarted", url), ("beforeRequestSent", final_url)]:
            intercepts.append(
                bidi.command(
                    "network.addIntercept",
                    {
                        "phases": [phase],
                        "urlPatterns": [{"type": "string", "pattern": target}],
                    },
                )["intercept"]
            )
        bidi.evaluate(
            context,
            f"""document.getElementById('interception-child').contentWindow
            .fetch({json.dumps(url)}, {{keepalive: true}}).catch(() => {{}}); 0""",
        )
        assert server.interception_received.wait(30), "The child request did not reach the response gate"
        bidi.evaluate(context, "document.getElementById('interception-child').remove(); 0")
        destroyed = bidi.event("browsingContext.contextDestroyed")
        assert destroyed["context"] == child, destroyed
        bidi.evaluate(context, "internals.gc(); undefined")
        server.release_interception.set()
        # NB: Fetch callbacks for the removed document need not run. Observe native redirect progress instead.
        fetch_records(bidi, send_fetch(bidi, context, f"{origin}/network-interception/barrier"))
        followed_redirect = server.detached_final_received.is_set()
        # NB: Automatic crash recovery can restore the page URL while replacing its script state.
        marker = bidi.evaluate(context, "window.detachedInterceptionMarker")
        assert marker["result"].get("value") is True, marker
        for event in bidi.events:
            if event["method"] in ["network.beforeRequestSent", "network.responseStarted"]:
                assert not event["params"]["isBlocked"], event
        bidi.events[:] = [
            event
            for event in bidi.events
            if event["method"] not in ["network.beforeRequestSent", "network.responseStarted"]
        ]
        if followed_redirect:
            print("Detached keepalive transport followed the redirect after context destruction.")
        else:
            print("Detached transport did not follow the redirect; stale-navigable counterproof was not reached.")

        # NB: The remaining live parent still intercepts its own request; detachment must not disable interception.
        pending = send_fetch(bidi, context, final_url)
        event = intercepted_event(bidi, "network.beforeRequestSent", final_url)
        bidi.command("network.continueRequest", {"request": event["request"]["request"]})
        fetch_records(bidi, pending)
        assert bidi.evaluate(context, "document.readyState")["result"]["value"] == "complete"
    finally:
        server.release_interception.set()
        if sys.exc_info()[0] is None:
            for intercept in intercepts:
                bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_authentication_gates(bidi, context, server, kinds, credentials=None):
    subscription = bidi.command("session.subscribe", {"events": ["network.authRequired"]})["subscription"]
    try:
        for kind in kinds:
            host = "localhost" if kind == "cors" else "127.0.0.1"
            url = f"http://{host}:{server.server_port}/network-interception/auth/gate-{kind}"
            intercept = bidi.command(
                "network.addIntercept",
                {
                    "phases": ["authRequired"],
                    "urlPatterns": [{"type": "string", "pattern": url}],
                },
            )["intercept"]
            with server.interception_lock:
                server.interception_requests.clear()
            fetch_options = {"credentials": "omit" if kind == "omit" else "include"}
            if kind != "default":
                fetch_options.update({"method": "POST", "body": "replayable-body"})
            options = json.dumps(fetch_options)
            if kind == "stream":
                options = "{method: 'POST', credentials: 'include', duplex: 'half', body: new ReadableStream({start(controller) {controller.enqueue(new Uint8Array([97])); controller.close();}})}"
            pending = bidi.send(
                "script.evaluate",
                {
                    "target": {"context": context},
                    "awaitPromise": True,
                    "expression": f"fetch({json.dumps(url)}, {options}).then(async response => JSON.stringify({{status: response.status, body: await response.text()}}), error => JSON.stringify({{error: error.name}}))",
                },
            )
            if kind in ["cancel", "default"]:
                event = intercepted_event(bidi, "network.authRequired", url)
                params = {"request": event["request"]["request"], "action": kind}
                if credentials is not None:
                    params["credentials"] = credentials
                bidi.command("network.continueWithAuth", params)
            else:
                # Observe an invalid block immediately, rather than waiting for the blocked fetch to time out.
                while pending not in bidi.responses:
                    assert not any(
                        event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
                        for event in bidi.events
                    ), bidi.events
                    bidi.read_message()
            result = fetch_records(bidi, pending)
            if kind == "stream":
                assert result == {"error": "TypeError"}, result
            elif kind == "default":
                # The browser's existing interactive credential prompt is not implemented.
                assert result == {"error": "AbortError"}, result
            else:
                assert result == {"status": 401, "body": "authentication required"}, result
            bidi.evaluate(context, "0")
            assert not any(
                event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
                for event in bidi.events
            ), bidi.events
            with server.interception_lock:
                records = list(server.interception_requests)
            assert len(records) == 1, records
            assert not any(key.lower() == "authorization" for key, _ in records[0]["headers"]), records
            if kind != "stream":
                assert records[0]["body"] == ("" if kind == "default" else "replayable-body"), records
            bidi.command("network.removeIntercept", {"intercept": intercept})
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_continue_response_auth(bidi, context, server, credentials):
    kind = "credentials" if credentials else "default"
    url = f"http://127.0.0.1:{server.server_port}/network-interception/auth/continue-response-{kind}"
    subscription = bidi.command("session.subscribe", {"events": ["network.authRequired"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {
            "phases": ["authRequired"],
            "urlPatterns": [{"type": "string", "pattern": url}],
        },
    )["intercept"]
    try:
        with server.interception_lock:
            server.interception_requests.clear()
        options = {"method": "POST", "body": "continue-response-body"} if credentials else {}
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "awaitPromise": True,
                "expression": f"fetch({json.dumps(url)}, {json.dumps(options)}).then(async response => JSON.stringify({{status: response.status, body: await response.text()}}), error => JSON.stringify({{error: error.name}}))",
            },
        )
        event = intercepted_event(bidi, "network.authRequired", url)
        params = {"request": event["request"]["request"]}
        if credentials:
            params["credentials"] = {"type": "password", "username": "user", "password": "pass"}
        bidi.command("network.continueResponse", params)
        while pending not in bidi.responses:
            assert not any(
                event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
                for event in bidi.events
            ), bidi.events
            bidi.read_message()
        result = fetch_records(bidi, pending)
        with server.interception_lock:
            records = list(server.interception_requests)
        if credentials:
            assert result["status"] == 200, result
            assert json.loads(result["body"]) == json.loads(json.dumps(records)), (result, records)
            assert len(records) == 2, records
            for record in records:
                assert record["method"] == "POST" and record["body"] == "continue-response-body", record
                lengths = [value for key, value in record["headers"] if key.lower() == "content-length"]
                assert lengths == [str(len("continue-response-body"))], record
            assert not any(key.lower() == "authorization" for key, _ in records[0]["headers"]), records
            authorization = [value for key, value in records[1]["headers"] if key.lower() == "authorization"]
            assert authorization == ["Basic dXNlcjpwYXNz"], records
        else:
            # Default authentication reaches the browser's existing unimplemented prompt fallback.
            assert result == {"error": "AbortError"}, result
            assert len(records) == 1 and records[0]["method"] == "GET", records
        bidi.evaluate(context, "0")
        assert not any(
            event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
            for event in bidi.events
        ), bidi.events
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_provide_response_auth(bidi, context, server, status):
    kind = "omitted" if status is None else str(status)
    url = f"http://127.0.0.1:{server.server_port}/network-interception/auth/provide-response-{kind}"
    subscription = bidi.command("session.subscribe", {"events": ["network.authRequired"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["authRequired"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        with server.interception_lock:
            server.interception_requests.clear()
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "awaitPromise": True,
                "expression": f"fetch({json.dumps(url)}).then(async response => JSON.stringify({{status: response.status, body: await response.text()}}), error => JSON.stringify({{error: error.name}}))",
            },
        )
        event = intercepted_event(bidi, "network.authRequired", url)
        params = {"request": event["request"]["request"], "body": {"type": "string", "value": "provided-auth-response"}}
        if status is not None:
            params["statusCode"] = status
        bidi.command("network.provideResponse", params)
        while pending not in bidi.responses:
            assert not any(
                event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
                for event in bidi.events
            ), bidi.events
            bidi.read_message()
        assert fetch_records(bidi, pending) == {
            "status": 401 if status is None else status,
            "body": "provided-auth-response",
        }
        with server.interception_lock:
            records = list(server.interception_requests)
        assert len(records) == 1 and records[0]["method"] == "GET", records
        assert not any(key.lower() == "authorization" for key, _ in records[0]["headers"]), records
        bidi.evaluate(context, "0")
        assert not any(
            event["method"] == "network.authRequired" and event["params"]["request"]["url"] == url
            for event in bidi.events
        ), bidi.events
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_failed_response_transport(bidi, context, server):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/fail-body"
    server.interception_transport_closed.clear()
    subscription = bidi.command("session.subscribe", {"events": ["network.responseStarted"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {
            "phases": ["responseStarted"],
            "urlPatterns": [{"type": "string", "pattern": url}],
        },
    )["intercept"]
    try:
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "awaitPromise": True,
                "expression": f"fetch({json.dumps(url)}).then(() => 'fulfilled', error => error.name)",
            },
        )
        event = intercepted_event(bidi, "network.responseStarted", url)
        bidi.command("network.failRequest", {"request": event["request"]["request"]})
        result = bidi.response(pending)
        assert result["type"] == "success" and result["result"]["result"]["value"] == "TypeError", result
        assert server.interception_transport_closed.wait(30), "Failed interception left the original transport open"
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_document_abort_releases_blocked_object(bidi, context, server):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/object-data"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        # An <object>'s fetch has no fetch record, and its document's abort still stops it, blocked or not.
        markup = f'<object data="{url}"></object>'
        bidi.evaluate(
            context,
            "window.objectFrame = document.createElement('iframe'); "
            f"objectFrame.srcdoc = {json.dumps(markup)}; document.body.append(objectFrame); 0",
        )
        event = intercepted_event(bidi, "network.beforeRequestSent", url)
        bidi.evaluate(context, "objectFrame.contentWindow.location = 'about:blank'; 0")
        deadline = time.monotonic() + 10
        while bidi.evaluate(context, "objectFrame.contentDocument.URL")["result"].get("value") != "about:blank":
            assert time.monotonic() < deadline, "The object's frame did not navigate away"
        for request_id in [event["request"]["request"], *take_blocked_requests(bidi, context, url)]:
            result = bidi.response(bidi.send("network.continueRequest", {"request": request_id}))
            assert result["type"] == "error" and result["error"] == "no such request", result
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            bidi.evaluate(context, "objectFrame.remove(); 0")


def check_network_abort_while_blocked_after_gc(bidi, context, server):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/abort-after-gc"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent", "network.fetchError"]})[
        "subscription"
    ]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "expression": f"window.interceptionAbort = new AbortController(); fetch({json.dumps(url)}, "
                "{signal: interceptionAbort.signal}).then(() => 'fulfilled', error => error.name)",
                "awaitPromise": True,
            },
        )
        event = intercepted_event(bidi, "network.beforeRequestSent", url)
        request_id = event["request"]["request"]
        # A collection while the request is blocked keeps its fetch, which the abort then ends once.
        bidi.evaluate(context, "internals.gc(); 0")
        bidi.evaluate(context, "interceptionAbort.abort(); 0")
        result = bidi.response(pending)
        assert result["type"] == "success" and result["result"]["result"]["value"] == "AbortError", result
        error = bidi.event("network.fetchError")
        while error["request"]["url"] != url:
            error = bidi.event("network.fetchError")
        assert error["request"]["request"] == request_id, error
        result = bidi.response(bidi.send("network.continueRequest", {"request": request_id}))
        assert result["type"] == "error" and result["error"] == "no such request", result

        # The aborted fetch's resource timing entry is reported after its error event.
        entry_count = f"performance.getEntriesByName({json.dumps(url)}).length"
        deadline = time.monotonic() + 10
        while not bidi.evaluate(context, entry_count)["result"].get("value"):
            assert time.monotonic() < deadline, "The aborted fetch reported no resource timing"
        assert not any(
            event["method"] == "network.fetchError" and event["params"]["request"]["url"] == url
            for event in bidi.events
        ), bidi.events
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_cancel_with_events_off(bidi, context, server, kind):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/events-off-{kind}"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent"]})["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        pending = None
        if kind == "abort":
            pending = bidi.send(
                "script.evaluate",
                {
                    "target": {"context": context},
                    "expression": f"window.interceptionAbort = new AbortController(); fetch({json.dumps(url)}, "
                    "{signal: interceptionAbort.signal}).then(() => 'fulfilled', error => error.name)",
                    "awaitPromise": True,
                },
            )
        else:
            markup = f'<object data="{url}"></object>'
            bidi.evaluate(
                context,
                "window.objectFrame = document.createElement('iframe'); "
                f"objectFrame.srcdoc = {json.dumps(markup)}; document.body.append(objectFrame); 0",
            )
        event = intercepted_event(bidi, "network.beforeRequestSent", url)
        request_id = event["request"]["request"]

        # With no network event subscribed the session stays active, and an abort or a stop still releases the request.
        bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
        subscription = None
        if kind == "abort":
            bidi.evaluate(context, "interceptionAbort.abort(); 0")
            result = bidi.response(pending)
            assert result["type"] == "success" and result["result"]["result"]["value"] == "AbortError", result
        else:
            bidi.evaluate(context, "objectFrame.contentWindow.stop(); 0")
        for blocked_id in [request_id, *take_blocked_requests(bidi, context, url)]:
            result = bidi.response(bidi.send("network.continueRequest", {"request": blocked_id}))
            assert result["type"] == "error" and result["error"] == "no such request", result
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            if subscription is not None:
                bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            if kind == "stop":
                bidi.evaluate(context, "objectFrame.remove(); 0")


def check_network_stopped_fetch_reports_error(bidi, context, server):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/fail-body"
    server.interception_transport_closed.clear()
    subscription = bidi.command(
        "session.subscribe",
        {"events": ["network.responseStarted", "network.responseCompleted", "network.fetchError"]},
    )["subscription"]
    try:
        # An <object>'s fetch has no fetch record, and stopping its frame still stops the fetch while its body arrives.
        markup = f'<object data="{url}"></object>'
        bidi.evaluate(
            context,
            "window.objectFrame = document.createElement('iframe'); "
            f"objectFrame.srcdoc = {json.dumps(markup)}; document.body.append(objectFrame); 0",
        )
        started = bidi.event("network.responseStarted")
        while started["request"]["url"] != url:
            started = bidi.event("network.responseStarted")
        bidi.evaluate(context, "objectFrame.contentWindow.stop(); 0")
        assert server.interception_transport_closed.wait(30), "The stopped fetch left its transport open"

        # A stop reports before it closes the transport, so the next reply follows every event the stop reports.
        bidi.evaluate(context, "0")
        request_ids = {started["request"]["request"]} | {
            event["params"]["request"]["request"]
            for event in bidi.events
            if event["method"] == "network.responseStarted" and event["params"]["request"]["url"] == url
        }
        for request_id in request_ids:
            terminals = [
                event["method"]
                for event in bidi.events
                if event["method"] in ["network.responseCompleted", "network.fetchError"]
                and event["params"]["request"]["request"] == request_id
            ]
            assert terminals == ["network.fetchError"], (request_id, terminals)
        bidi.events = [
            event
            for event in bidi.events
            if not event["method"].startswith("network.") or event["params"]["request"]["url"] != url
        ]
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            bidi.evaluate(context, "objectFrame.remove(); 0")


def check_network_continue_after_gc(bidi, context, server):
    subscription = bidi.command(
        "session.subscribe",
        {"events": ["network.beforeRequestSent", "network.responseStarted", "network.authRequired"]},
    )["subscription"]
    continue_commands = {
        "beforeRequestSent": "network.continueRequest",
        "responseStarted": "network.continueResponse",
        "authRequired": "network.continueWithAuth",
    }
    server.release_preflight.set()
    try:
        for phase in [*continue_commands, "preflight"]:
            if phase == "preflight":
                # A cross-origin PUT blocks first for its preflight, then for itself.
                url = f"http://localhost:{server.server_port}/network-interception/cors/continue-after-gc"
                options = {"method": "PUT", "headers": {"X-BiDi": "1"}}
                blocked = [("beforeRequestSent", "OPTIONS"), ("beforeRequestSent", "PUT")]
            else:
                auth = "auth/" if phase == "authRequired" else ""
                url = f"http://127.0.0.1:{server.server_port}/network-interception/{auth}continue-after-gc-{phase}"
                options = None
                blocked = [(phase, "GET")]
            intercept = bidi.command(
                "network.addIntercept",
                {
                    "phases": sorted({blocked_phase for blocked_phase, _ in blocked}),
                    "urlPatterns": [{"type": "string", "pattern": url}],
                },
            )["intercept"]
            with server.interception_lock:
                server.interception_requests.clear()
            pending = send_fetch(bidi, context, url, options)
            for blocked_phase, method in blocked:
                event = intercepted_event(bidi, f"network.{blocked_phase}", url)
                assert event["request"]["method"] == method, event
                # A collection while the request is blocked keeps what its continuation needs.
                bidi.evaluate(context, "internals.gc(); undefined")
                params = {"request": event["request"]["request"]}
                if blocked_phase == "authRequired":
                    params["action"] = "provideCredentials"
                    params["credentials"] = {"type": "password", "username": "user", "password": "pass"}
                bidi.command(continue_commands[blocked_phase], params)
            records = fetch_records(bidi, pending)
            path = urllib.parse.urlsplit(url).path
            expected = (
                [(path, "PUT")] if phase == "preflight" else [(path, "GET")] * (2 if phase == "authRequired" else 1)
            )
            assert [(record["path"], record["method"]) for record in records] == expected, records
            if phase == "authRequired":
                authorization = [value for key, value in records[-1]["headers"] if key.lower() == "authorization"]
                assert authorization == ["Basic dXNlcjpwYXNz"], records
            bidi.command("network.removeIntercept", {"intercept": intercept})
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_intercepted_adopted_navigation(bidi, context, server, action):
    # A cross-site navigation blocked at responseStarted commits its document in another process once continued.
    terminal_methods = {"network.responseCompleted", "network.fetchError"}
    origin = f"http://127.0.0.1:{server.server_port}"
    url = f"http://localhost:{server.server_port}/network-document/intercepted-{action}"
    provided = "<!doctype html><title>Provided document</title><p>Provided"
    server.document_headers_sent.clear()
    server.release_document.clear()
    server.interception_transport_closed.clear()
    subscription = bidi.command(
        "session.subscribe",
        {"events": ["network.responseStarted", *sorted(terminal_methods)], "contexts": [context]},
    )["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["responseStarted"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]

    def is_our_terminal_event(event):
        return event["method"] in terminal_methods and event["params"].get("request", {}).get("url") == url

    def next_terminal_event():
        while True:
            for index, event in enumerate(bidi.events):
                if is_our_terminal_event(event):
                    return bidi.events.pop(index)
            bidi.read_message()

    def wait_for_commit():
        deadline = time.monotonic() + 10
        while True:
            result = bidi.response(
                bidi.send(
                    "script.evaluate",
                    {"target": {"context": context}, "expression": "document.URL", "awaitPromise": False},
                )
            )
            if result["type"] == "success" and result["result"]["result"].get("value") == url:
                return
            assert time.monotonic() < deadline, "The navigation did not commit its document"

    try:
        navigation = bidi.send("browsingContext.navigate", {"context": context, "url": url, "wait": "none"})
        event = intercepted_event(bidi, "network.responseStarted", url)
        request_id = event["request"]["request"]
        assert server.document_headers_sent.wait(10)
        expected_size = len(provided) if action == "provide" else len(server.document_body)
        if action == "continue":
            bidi.command("network.continueResponse", {"request": request_id})
            wait_for_commit()
            server.release_document.set()
        elif action == "fail":
            bidi.command("network.failRequest", {"request": request_id})
        else:
            bidi.command(
                "network.provideResponse",
                {"request": request_id, "body": {"type": "string", "value": provided}},
            )
            wait_for_commit()
            assert bidi.evaluate(context, "document.title")["result"]["value"] == "Provided document"
        # A failed navigation settles too, with an error document, before the next navigation starts.
        result = bidi.response(navigation)
        if action != "fail":
            assert result["type"] == "success", result

        terminal = next_terminal_event()
        params = terminal["params"]
        assert params["request"]["request"] == request_id, params
        if action == "fail":
            assert terminal["method"] == "network.fetchError", terminal
        else:
            assert terminal["method"] == "network.responseCompleted", terminal
            assert params["response"]["bodySize"] == expected_size, params
            assert params["response"]["content"]["size"] == expected_size, params
        if action != "continue":
            assert server.interception_transport_closed.wait(30), "The original response's transport stayed open"

        # The navigation's body ends once, in whichever process holds it, and the process that handed it over reports
        # nothing more for it, so a reply from the document that holds it follows every event of the request.
        bidi.evaluate(context, "0")
        assert not any(is_our_terminal_event(event) for event in bidi.events), bidi.events
    finally:
        server.release_document.set()
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            bidi.command(
                "browsingContext.navigate",
                {"context": context, "url": f"{origin}/interception-page", "wait": "complete"},
            )


def check_network_provided_body_at_response_started(bidi, context, server):
    url = f"http://127.0.0.1:{server.server_port}/network-interception/fail-body"
    provided = "provided body"
    server.interception_transport_closed.clear()
    subscription = bidi.command(
        "session.subscribe", {"events": ["network.responseStarted", "network.responseCompleted"]}
    )["subscription"]
    intercept = bidi.command(
        "network.addIntercept",
        {
            "phases": ["responseStarted"],
            "urlPatterns": [{"type": "string", "pattern": url}],
        },
    )["intercept"]
    try:
        pending = bidi.send(
            "script.evaluate",
            {
                "target": {"context": context},
                "awaitPromise": True,
                "expression": f"fetch({json.dumps(url)}).then(response => response.text())",
            },
        )
        # The original body is still arriving when the provided one replaces it.
        event = intercepted_event(bidi, "network.responseStarted", url)
        bidi.command(
            "network.provideResponse",
            {"request": event["request"]["request"], "body": {"type": "string", "value": provided}},
        )
        result = bidi.response(pending)
        assert result["type"] == "success" and result["result"]["result"]["value"] == provided, result
        assert server.interception_transport_closed.wait(30), "The provided body left the original transport open"
        completed = bidi.event("network.responseCompleted")
        while completed["request"]["url"] != url:
            completed = bidi.event("network.responseCompleted")
        response = completed["response"]
        assert response["bodySize"] == len(provided), response
        assert response["content"]["size"] == len(provided), response
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_scoped_headers(bidi, context, server):
    other = bidi.command("browsingContext.create", {"type": "tab"})["context"]

    def set_headers(target, value):
        bidi.command(
            "network.setExtraHeaders",
            {
                "contexts": [target],
                "headers": [{"name": "X-Scoped-Secret", "value": {"type": "string", "value": value}}],
            },
        )

    def check_headers(target, host, expected):
        with server.interception_lock:
            server.interception_requests.clear()
        records = fetch_records(
            bidi, send_fetch(bidi, target, f"http://{host}:{server.server_port}/network-interception/scoped")
        )
        values = [value for key, value in records[-1]["headers"] if key.lower() == "x-scoped-secret"]
        assert values == [expected], records

    try:
        set_headers(context, "first-tab")
        # Set the second tab's scope before its new process is provisioned.
        set_headers(other, "second-tab")
        bidi.command(
            "browsingContext.navigate",
            {
                "context": other,
                "url": f"http://localhost:{server.server_port}/input-child",
                "wait": "complete",
            },
        )
        bidi.evaluate(
            other,
            "new Promise(resolve => { const frame = document.createElement('iframe'); "
            f"frame.src = 'http://127.0.0.1:{server.server_port}/input-child'; "
            "frame.onload = resolve; document.body.append(frame); })",
            awaitPromise=True,
        )
        tree = bidi.command("browsingContext.getTree", {"root": other})["contexts"][0]
        assert len(tree["children"]) == 1, tree
        child = tree["children"][0]["context"]
        check_headers(context, "127.0.0.1", "first-tab")
        check_headers(other, "localhost", "second-tab")
        check_headers(child, "127.0.0.1", "second-tab")
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.setExtraHeaders", {"contexts": [context], "headers": []})
            bidi.command("browsingContext.close", {"context": other})


def check_network_intercepted_navigation(bidi, context, server):
    origin = f"http://127.0.0.1:{server.server_port}"
    url = f"http://localhost:{server.server_port}/intercepted-navigation"
    subscription = bidi.command("session.subscribe", {"events": ["network.beforeRequestSent", "browsingContext.load"]})[
        "subscription"
    ]
    intercept = bidi.command(
        "network.addIntercept",
        {"phases": ["beforeRequestSent"], "urlPatterns": [{"type": "string", "pattern": url}]},
    )["intercept"]
    try:
        navigation_command = bidi.send("browsingContext.navigate", {"context": context, "url": url, "wait": "none"})
        event = intercepted_event(bidi, "network.beforeRequestSent", url)
        assert event["navigation"] is not None, event
        bidi.command(
            "network.provideResponse",
            {
                "request": event["request"]["request"],
                "statusCode": 200,
                "headers": [{"name": "Content-Type", "value": {"type": "string", "value": "text/html"}}],
                "body": {"type": "string", "value": "<!doctype html><title>Intercepted navigation</title><p>Provided"},
            },
        )
        navigation = bidi.response(navigation_command)
        assert navigation["type"] == "success", navigation
        assert navigation["result"]["navigation"] == event["navigation"], navigation
        loaded = bidi.event("browsingContext.load")
        assert loaded["context"] == context and loaded["url"] == url, loaded
        assert loaded["navigation"] == event["navigation"], loaded
        assert bidi.evaluate(context, "document.title")["result"]["value"] == "Intercepted navigation"
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("network.removeIntercept", {"intercept": intercept})
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            bidi.command(
                "browsingContext.navigate",
                {"context": context, "url": f"{origin}/interception-page", "wait": "complete"},
            )


def check_network_interception(bidi, context, server, case="all"):
    bidi.command(
        "browsingContext.navigate",
        {
            "context": context,
            "url": f"http://127.0.0.1:{server.server_port}/interception-page",
            "wait": "complete",
        },
    )
    # NB: This must run before any network subscription or interception creates state for the held request.
    if case in ["all", "late-enable"]:
        check_network_late_interception(bidi, context, server)
    if case in ["all", "late-abort"]:
        check_network_late_interception(bidi, context, server, abort=True)
    if case in ["all", "redirect", "auth", "overrides"]:
        check_network_override_retries(
            bidi, context, server, ["redirect", "auth", "overrides"] if case == "all" else [case]
        )
    echo_cases = [
        "echo-307",
        "echo-303",
        "echo-auth",
        "explicit-307",
        "explicit-303",
        "explicit-auth",
        "echo-body",
        "echo-range",
        "explicit-range",
        "explicit-unsubscribe",
    ]
    if case in ["all", "echo-headers", *echo_cases]:
        check_network_echoed_headers(bidi, context, server, echo_cases if case in ["all", "echo-headers"] else [case])
    if case in ["all", "detached"]:
        check_network_detached_interception(bidi, context, server)
    if case in ["all", "navigation"]:
        check_network_intercepted_navigation(bidi, context, server)
    if case in ["all", "scoped-headers"]:
        check_network_scoped_headers(bidi, context, server)
    if case in ["all", "fail-transport"]:
        check_network_failed_response_transport(bidi, context, server)
    if case in ["all", "provided-body-at-response-started"]:
        check_network_provided_body_at_response_started(bidi, context, server)
    if case in ["all", "document-abort-object"]:
        check_network_document_abort_releases_blocked_object(bidi, context, server)
    if case in ["all", "abort-after-gc"]:
        check_network_abort_while_blocked_after_gc(bidi, context, server)
    for kind in ["abort", "stop"]:
        if case in ["all", f"events-off-{kind}"]:
            check_network_cancel_with_events_off(bidi, context, server, kind)
    if case in ["all", "events-on-stop"]:
        check_network_stopped_fetch_reports_error(bidi, context, server)
    if case in ["all", "continue-after-gc"]:
        check_network_continue_after_gc(bidi, context, server)
    for action in ["continue", "fail", "provide"]:
        if case in ["all", f"adopted-navigation-{action}"]:
            check_network_intercepted_adopted_navigation(bidi, context, server, action)
    for action in ["cancel", "default"]:
        if case in ["all", "auth-response-matrix", "auth-action-extras", f"auth-{action}-extra"]:
            for credentials in [{"type": "password", "username": "user", "password": "pass"}, {}]:
                check_network_authentication_gates(bidi, context, server, [action], credentials)
    for status, kind in [(None, "omitted"), (401, "401"), (200, "200")]:
        if case in ["all", "auth-response-matrix", "auth-provide-response", f"auth-provide-response-{kind}"]:
            check_network_provide_response_auth(bidi, context, server, status)
    if case in ["all", "auth-response-matrix", "auth-continue-response", "auth-continue-response-default"]:
        check_network_continue_response_auth(bidi, context, server, credentials=False)
    if case in ["all", "auth-response-matrix", "auth-continue-response", "auth-continue-response-credentials"]:
        check_network_continue_response_auth(bidi, context, server, credentials=True)
    if case in ["all", "auth-gates", "auth-omit", "auth-cors", "auth-stream", "auth-cancel", "auth-default"]:
        kinds = (
            ["omit", "cors", "stream", "cancel", "default"]
            if case in ["all", "auth-gates"]
            else [case.removeprefix("auth-")]
        )
        check_network_authentication_gates(bidi, context, server, kinds)


def check_network_download_events(bidi, context, server, downloads):
    if sys.platform == "win32":
        print("Skipping download event tests: Windows does not support the temporary download directory override.")
        return
    terminal_methods = {"network.responseCompleted", "network.fetchError"}
    subscription = bidi.command(
        "session.subscribe",
        {
            "events": ["network.beforeRequestSent", "network.responseStarted", *sorted(terminal_methods)],
            "contexts": [context],
        },
    )["subscription"]
    try:
        for kind in ["success", "truncated", "split"]:
            url = f"http://127.0.0.1:{server.server_port}/network-download/{kind}"
            server.download_headers_sent.clear()
            server.release_download.clear()
            if kind == "split":
                server.segmented_download_body = bytes(range(256)) * (8 * 1024 * 1024 // 256)
                server.range_request_received.clear()
                server.release_initial_download.clear()
                server.initial_download_stopped.clear()
            expected_body = server.segmented_download_body if kind == "split" else server.download_body

            def is_our_event(event, expected_url=url):
                return event["params"].get("request", {}).get("url") == expected_url

            def next_event(methods):
                while True:
                    for index, event in enumerate(bidi.events):
                        if event["method"] in methods and is_our_event(event):
                            return bidi.events.pop(index)
                    bidi.read_message()

            try:
                # Downloads do not commit documents; observe network events without awaiting the commit waiter.
                bidi.send("browsingContext.navigate", {"context": context, "url": url, "wait": "none"})
                before = next_event({"network.beforeRequestSent"})["params"]
                started = next_event({"network.responseStarted", *terminal_methods})
                assert started["method"] == "network.responseStarted", started
                started = started["params"]
                assert server.download_headers_sent.wait(10)
                request_id = started["request"]["request"]
                assert isinstance(request_id, str) and request_id, started
                assert before["request"]["request"] == request_id, (before, started)
                assert isinstance(started["navigation"], str), started
                assert started["navigation"] == before["navigation"], (before, started)

                if kind == "split":
                    assert server.range_request_received.wait(10), (
                        "The downloader did not split its range-capable response"
                    )
                    server.release_initial_download.set()
                    assert server.initial_download_stopped.wait(10), "The initial segment did not stop"

                # The initial segment may be stopped, but completion must wait for the held remaining ranges.
                bidi.evaluate(context, "0")
                assert not any(event["method"] in terminal_methods and is_our_event(event) for event in bidi.events), (
                    bidi.events
                )
                assert not server.release_download.is_set()

                server.release_download.set()
                terminal = next_event(terminal_methods)
                expected_method = "network.fetchError" if kind == "truncated" else "network.responseCompleted"
                assert terminal["method"] == expected_method, terminal
                params = terminal["params"]
                assert params["context"] == context, params
                assert params["request"]["request"] == request_id, params
                assert params["navigation"] == started["navigation"], params
                assert params["redirectCount"] == started["redirectCount"] == 0, params
                if kind != "truncated":
                    response = params["response"]
                    assert response["bodySize"] == len(expected_body), response
                    assert response["content"]["size"] == len(expected_body), response
                    assert response["bytesReceived"] == response["headersSize"] + len(expected_body), response
                    timings = params["request"]["timings"]
                    assert timings["responseEnd"] >= timings["responseStart"] >= timings["fetchStart"], timings
                else:
                    assert isinstance(params["errorText"], str) and params["errorText"], params

                # The transport's finish callback also finalizes the download before the next UI/renderer barrier.
                bidi.evaluate(context, "0")
                assert not any(event["method"] in terminal_methods and is_our_event(event) for event in bidi.events), (
                    bidi.events
                )
                path = os.path.join(downloads, f"bidi-{kind}.bin")
                if kind != "truncated":
                    with open(path, "rb") as downloaded:
                        assert downloaded.read() == expected_body
                else:
                    assert not os.path.exists(path), path
            finally:
                server.release_initial_download.set()
                server.release_download.set()
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_network_adopted_navigation_events(bidi, context, server):
    # A cross-site navigation commits its document in another process, which adopts the body from RequestServer.
    terminal_methods = {"network.responseCompleted", "network.fetchError"}
    bidi.command(
        "browsingContext.navigate",
        {"context": context, "url": f"http://127.0.0.1:{server.server_port}/reload", "wait": "complete"},
    )
    subscription = bidi.command(
        "session.subscribe",
        {
            "events": ["network.beforeRequestSent", "network.responseStarted", *sorted(terminal_methods)],
            "contexts": [context],
        },
    )["subscription"]
    try:
        for kind in ["success", "truncated", "stopped"]:
            url = f"http://localhost:{server.server_port}/network-document/{kind}"
            server.document_headers_sent.clear()
            server.release_document.clear()

            def is_our_event(event, expected_url=url):
                return event["params"].get("request", {}).get("url") == expected_url

            def next_event(methods):
                while True:
                    for index, event in enumerate(bidi.events):
                        if event["method"] in methods and is_our_event(event):
                            return bidi.events.pop(index)
                    bidi.read_message()

            try:
                bidi.send("browsingContext.navigate", {"context": context, "url": url, "wait": "none"})
                before = next_event({"network.beforeRequestSent"})["params"]
                started = next_event({"network.responseStarted", *terminal_methods})
                assert started["method"] == "network.responseStarted", started
                started = started["params"]
                assert server.document_headers_sent.wait(10)
                request_id = started["request"]["request"]
                assert before["request"]["request"] == request_id, (before, started)

                # The document commits while the rest of its body is held.
                deadline = time.monotonic() + 10
                while True:
                    result = bidi.response(
                        bidi.send(
                            "script.evaluate",
                            {"target": {"context": context}, "expression": "document.URL", "awaitPromise": False},
                        )
                    )
                    if result["type"] == "success" and result["result"]["result"].get("value") == url:
                        break
                    assert time.monotonic() < deadline, "The navigation did not commit its document"
                assert not any(event["method"] in terminal_methods and is_our_event(event) for event in bidi.events), (
                    bidi.events
                )

                if kind == "stopped":
                    # A stop in the document's process ends the adopted body with a fetch error.
                    bidi.evaluate(context, "window.stop()")
                else:
                    server.release_document.set()
                terminal = next_event(terminal_methods)
                expected_method = "network.responseCompleted" if kind == "success" else "network.fetchError"
                assert terminal["method"] == expected_method, terminal
                params = terminal["params"]
                assert params["context"] == context, params
                assert params["request"]["request"] == request_id, params
                assert params["navigation"] == started["navigation"], params
                if kind == "success":
                    response = params["response"]
                    assert response["bodySize"] == len(server.document_body), response
                    assert response["content"]["size"] == len(server.document_body), response
                    assert response["bytesReceived"] == response["headersSize"] + len(server.document_body), response
                else:
                    assert isinstance(params["errorText"], str) and params["errorText"], params

                # Only the document's process reports the end of its body, and only once.
                bidi.evaluate(context, "0")
                assert not any(event["method"] in terminal_methods and is_our_event(event) for event in bidi.events), (
                    bidi.events
                )
            finally:
                server.release_document.set()
    finally:
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
            bidi.command("browsingContext.navigate", {"context": context, "url": "about:blank", "wait": "complete"})


def check_network_abort_before_transport(bidi, context, server):
    # A cross-origin fetch waits for its CORS preflight before its own request starts, so it has no network transport.
    terminal_methods = {"network.responseCompleted", "network.fetchError"}
    url = f"http://localhost:{server.server_port}/network-preflight"
    bidi.command(
        "browsingContext.navigate",
        {"context": context, "url": f"http://127.0.0.1:{server.server_port}/reload", "wait": "complete"},
    )
    subscription = bidi.command(
        "session.subscribe",
        {"events": ["network.beforeRequestSent", *sorted(terminal_methods)], "contexts": [context]},
    )["subscription"]
    server.preflight_received.clear()
    server.release_preflight.clear()

    def is_our_event(event):
        request = event["params"].get("request", {})
        return request.get("url") == url and request.get("method") == "PUT"

    try:
        bidi.evaluate(
            context,
            f"window.bidiAbort = new AbortController(); fetch('{url}', {{ method: 'PUT', headers: {{ 'X-BiDi': '1' }}, signal: window.bidiAbort.signal }}).catch(() => {{}}); 0",
        )
        assert server.preflight_received.wait(10), "The fetch did not send its preflight"
        bidi.evaluate(context, "window.bidiAbort.abort(); 0")
        while True:
            terminal = next(
                (event for event in bidi.events if event["method"] in terminal_methods and is_our_event(event)), None
            )
            if terminal is not None:
                bidi.events.remove(terminal)
                break
            bidi.read_message()
        assert terminal["method"] == "network.fetchError", terminal
        assert isinstance(terminal["params"]["errorText"], str) and terminal["params"]["errorText"], terminal

        # Releasing the preflight reports nothing more for the aborted request. The request then ends with a network
        # error, whose report would come before the request's resource timing entry.
        entry_count = f"performance.getEntriesByName({json.dumps(url)}).length"
        assert not bidi.evaluate(context, entry_count)["result"].get("value"), "The aborted request ended early"
        server.release_preflight.set()
        deadline = time.monotonic() + 10
        while not bidi.evaluate(context, entry_count)["result"].get("value"):
            assert time.monotonic() < deadline, "The aborted request reported no resource timing"
        assert not any(event["method"] in terminal_methods and is_our_event(event) for event in bidi.events), (
            bidi.events
        )
    finally:
        server.release_preflight.set()
        if sys.exc_info()[0] is None:
            bidi.command("session.unsubscribe", {"subscriptions": [subscription]})


def check_screenshot_handles(bidi, context):
    bidi.command("browsingContext.activate", {"context": context})
    element = bidi.evaluate(context, "document.body.style.height = '20px'; document.body")["result"]
    params = {
        "context": context,
        "clip": {"type": "element", "element": {"sharedId": element["sharedId"]}},
        "imageSize": {"maxWidth": 1, "maxHeight": 1},
    }
    for handle in [42, None, {}]:
        params["clip"]["element"]["handle"] = handle
        result = bidi.response(bidi.send("browsingContext.captureScreenshot", params))
        assert result["type"] == "error" and result["error"] == "invalid argument", result
    for reference in [{"sharedId": element["sharedId"]}, {"sharedId": element["sharedId"], "handle": "ignored"}]:
        params["clip"]["element"] = reference
        result = bidi.command("browsingContext.captureScreenshot", params)
        assert base64.b64decode(result["data"]).startswith(b"\x89PNG\r\n\x1a\n"), result


def check_input_handles(bidi, context):
    element = bidi.evaluate(context, "document.body")["result"]
    for source_type, action in [
        ("pointer", {"type": "pointerMove", "x": 0, "y": 0}),
        ("wheel", {"type": "scroll", "x": 0, "y": 0, "deltaX": 0, "deltaY": 0}),
    ]:
        origin = {"type": "element", "element": {"sharedId": element["sharedId"]}}
        params = {
            "context": context,
            "actions": [{"type": source_type, "id": source_type, "actions": [{**action, "origin": origin}]}],
        }
        try:
            for handle in [42, None, {}]:
                origin["element"]["handle"] = handle
                result = bidi.response(bidi.send("input.performActions", params))
                assert result["type"] == "error" and result["error"] == "invalid argument", result
            for reference in [
                {"sharedId": element["sharedId"]},
                {"sharedId": element["sharedId"], "handle": "ignored"},
            ]:
                origin["element"] = reference
                bidi.command("input.performActions", params)
        finally:
            bidi.command("input.releaseActions", {"context": context})


def check_input_queue(bidi, context):
    bidi.evaluate(
        context,
        "window.inputEvents = []; document.onkeydown = event => inputEvents.push(event.type); document.onkeyup = event => inputEvents.push(event.type)",
    )
    try:
        commands = [
            bidi.send(
                "input.performActions",
                {
                    "context": context,
                    "actions": [{"type": "key", "id": "keyboard", "actions": [{"type": "keyDown", "value": "a"}]}],
                },
            ),
            bidi.send("input.performActions", {"context": context, "actions": []}),
            bidi.send("input.releaseActions", {"context": context}),
        ]
        for command in commands:
            result = bidi.response(command)
            assert result["type"] == "success", result
        assert bidi.evaluate(context, "inputEvents.join(',')")["result"]["value"] == "keydown,keyup"
    finally:
        bidi.evaluate(context, "document.onkeydown = document.onkeyup = null; delete window.inputEvents")
        bidi.command("input.releaseActions", {"context": context})


def check_input_cross_context(bidi, connection, session_id, parent, server):
    parent_url = f"http://127.0.0.1:{server.server_port}/input-parent"
    child_url = f"http://localhost:{server.server_port}/input-child"
    bidi.command("browsingContext.navigate", {"context": parent, "url": parent_url, "wait": "complete"})
    tree = bidi.command("browsingContext.getTree", {"root": parent})["contexts"][0]
    assert len(tree["children"]) == 1, tree
    child = tree["children"][0]["context"]
    assert tree["children"][0]["url"] == child_url, tree
    other = bidi.command("browsingContext.create", {"type": "tab", "background": True})["context"]
    bidi.command(
        "browsingContext.navigate",
        {"context": other, "url": f"http://127.0.0.1:{server.server_port}/input-child", "wait": "complete"},
    )

    def focus(target):
        bidi.command("browsingContext.activate", {"context": parent if target == child else target})
        if target == child:
            bidi.evaluate(parent, "document.getElementById('input-child').focus(); 0")
        result = bidi.evaluate(target, "document.getElementById('target').focus(); document.hasFocus()")
        assert result["result"]["value"] is True, result

    def events(target):
        return json.loads(bidi.evaluate(target, "JSON.stringify(inputEvents)")["result"]["value"])

    def clear_events():
        for target in [parent, child, other]:
            bidi.evaluate(target, "inputEvents = []; 0")

    def key_sequence(*actions, source_id="shared-keyboard"):
        return [
            {"type": "key", "id": source_id, "actions": [{"type": kind, "value": value} for kind, value in actions]}
        ]

    def keys(target, *actions, source_id="shared-keyboard"):
        bidi.command(
            "input.performActions", {"context": target, "actions": key_sequence(*actions, source_id=source_id)}
        )

    def release(target):
        bidi.command("input.releaseActions", {"context": target})

    def classic(method, suffix, payload=None):
        status, result, _ = helpers.request(connection, method, f"/session/{session_id}/{suffix}", payload)
        assert status == 200, result
        return result["value"]

    for target in [parent, child, other]:
        bidi.evaluate(
            target,
            """window.inputEvents = [];
            for (const type of ['keydown', 'keyup']) {
                document.addEventListener(type, event => inputEvents.push([
                    event.type, event.key, event.shiftKey, event.repeat
                ]));
            }""",
        )

    try:
        # Releasing from an OOP child must release keys pressed in its parent and reset the shared source.
        focus(parent)
        keys(parent, ("keyDown", "a"))
        assert events(parent) == [["keydown", "a", False, False]], events(parent)
        focus(child)
        release(child)
        assert events(child) == [["keyup", "a", False, False]], (events(parent), events(child))
        focus(parent)
        keys(parent, ("keyDown", "a"))
        assert events(parent) == [["keydown", "a", False, False], ["keydown", "a", False, False]], events(parent)
        release(parent)

        # Updates from both processes accumulate, including the reverse-order cancel list.
        clear_events()
        keys(parent, ("keyDown", "a"))
        focus(child)
        keys(child, ("keyDown", "b"))
        focus(parent)
        release(parent)
        assert events(parent) == [
            ["keydown", "a", False, False],
            ["keyup", "b", False, False],
            ["keyup", "a", False, False],
        ], events(parent)
        focus(child)
        keys(child, ("keyDown", "b"))
        assert events(child) == [["keydown", "b", False, False], ["keydown", "b", False, False]], events(child)
        release(child)

        # Modifiers cross frame processes, but another top-level tab has an independent input state.
        clear_events()
        focus(parent)
        keys(parent, ("keyDown", "\ue008"))
        focus(other)
        keys(other, ("keyDown", "a"))
        assert events(other) == [["keydown", "a", False, False]], events(other)
        release(other)
        focus(child)
        keys(child, ("keyDown", "a"))
        assert events(child) == [["keydown", "A", True, False]], events(child)
        release(child)
        assert events(child) == [
            ["keydown", "A", True, False],
            ["keyup", "A", True, False],
            ["keyup", "Shift", False, False],
        ], events(child)
        focus(parent)
        keys(parent, ("keyDown", "a"))
        assert events(parent)[-1] == ["keydown", "a", False, False], events(parent)
        release(parent)

        # A source ID has one type throughout a tab, including when a different process parses the next command.
        clear_events()
        keys(parent, ("keyDown", "c"), source_id="cross-context-source")
        conflicting = [{"type": "pointer", "id": "cross-context-source", "actions": [{"type": "pause"}]}]
        result = bidi.response(bidi.send("input.performActions", {"context": child, "actions": conflicting}))
        assert result["type"] == "error" and result["error"] == "invalid argument", result
        bidi.command("input.performActions", {"context": other, "actions": conflicting})
        release(other)
        focus(child)
        release(child)
        assert events(child) == [["keyup", "c", False, False]], events(child)

        # Mutations before a dispatch error must still reach the shared state and its cancel list.
        clear_events()
        focus(parent)
        result = bidi.response(
            bidi.send(
                "input.performActions",
                {
                    "context": parent,
                    "actions": [
                        {
                            "type": "key",
                            "id": "partial-keyboard",
                            "actions": [{"type": "keyDown", "value": "d"}, {"type": "pause"}],
                        },
                        {
                            "type": "pointer",
                            "id": "partial-pointer",
                            "actions": [{"type": "pause"}, {"type": "pointerMove", "x": -1, "y": 0}],
                        },
                    ],
                },
            )
        )
        assert result["type"] == "error" and result["error"] == "move target out of bounds", result
        assert events(parent) == [["keydown", "d", False, False]], events(parent)
        focus(child)
        release(child)
        assert events(child) == [["keyup", "d", False, False]], events(child)
        focus(parent)
        keys(parent, ("keyDown", "d"), source_id="partial-keyboard")
        assert events(parent)[-1] == ["keydown", "d", False, False], events(parent)
        release(parent)

        # Classic actions in the child and BiDi actions in the parent use the same state in both directions.
        classic("POST", "window", {"handle": parent})
        classic("POST", "frame", {"id": None})
        frame = classic("POST", "element", {"using": "css selector", "value": "#input-child"})
        classic("POST", "frame", {"id": frame})
        clear_events()
        focus(parent)
        keys(parent, ("keyDown", "e"))
        focus(child)
        classic("DELETE", "actions")
        assert events(child) == [["keyup", "e", False, False]], events(child)
        focus(parent)
        keys(parent, ("keyDown", "e"), ("keyUp", "e"))
        assert events(parent)[-2:] == [["keydown", "e", False, False], ["keyup", "e", False, False]], events(parent)
        clear_events()
        focus(child)
        classic("POST", "actions", {"actions": key_sequence(("keyDown", "f"))})
        focus(parent)
        release(parent)
        assert events(parent) == [["keyup", "f", False, False]], events(parent)
        focus(child)
        classic("POST", "actions", {"actions": key_sequence(("keyDown", "f"), ("keyUp", "f"))})
        assert events(child)[-2:] == [["keydown", "f", False, False], ["keyup", "f", False, False]], events(child)
        release(child)
        classic("POST", "frame", {"id": None})

        # Wait for a DOM event before submitting the child command during the parent's protocol pause.
        # The assertions concern event/state order, not how long the pause or the IPC takes.
        clear_events()
        focus(parent)
        bidi.evaluate(
            parent,
            "window.inputKeydown = new Promise(resolve => document.addEventListener('keydown', event => resolve(event.key), {once: true})); 0",
        )
        barrier = bidi.send(
            "script.evaluate",
            {"target": {"context": parent}, "expression": "inputKeydown", "awaitPromise": True},
        )
        first = bidi.send(
            "input.performActions",
            {
                "context": parent,
                "actions": [
                    {
                        "type": "key",
                        "id": "queued-keyboard",
                        "actions": [{"type": "keyDown", "value": "q"}, {"type": "pause", "duration": 1000}],
                    }
                ],
            },
        )
        result = bidi.response(barrier)
        assert result["type"] == "success" and result["result"]["result"]["value"] == "q", result
        focus(child)
        queued_release = bidi.send("input.releaseActions", {"context": child})
        for command in [first, queued_release]:
            result = bidi.response(command)
            assert result["type"] == "success", result
        assert events(child) == [["keyup", "q", False, False]], events(child)
        focus(parent)
        keys(parent, ("keyDown", "q"), source_id="queued-keyboard")
        assert events(parent)[-1] == ["keydown", "q", False, False], events(parent)
        release(parent)
    finally:
        classic("POST", "window", {"handle": parent})
        classic("POST", "frame", {"id": None})
        for target in [child, parent, other]:
            release(target)
        bidi.command("browsingContext.close", {"context": other})
        bidi.command("browsingContext.activate", {"context": parent})
        bidi.command("browsingContext.navigate", {"context": parent, "url": "about:blank", "wait": "complete"})


def check_input_click_navigation(bidi, connection, session_id, parent, server):
    parent_url = f"http://127.0.0.1:{server.server_port}/input-parent"
    child_url = f"http://localhost:{server.server_port}/input-child"
    replacement_url = f"http://localhost:{server.server_port}/input-child"
    bidi.command("browsingContext.navigate", {"context": parent, "url": parent_url, "wait": "complete"})
    tree = bidi.command("browsingContext.getTree", {"root": parent})["contexts"][0]
    assert len(tree["children"]) == 1, tree
    child = tree["children"][0]["context"]
    assert tree["children"][0]["url"] == child_url, tree

    def classic(method, suffix, payload=None):
        status, result, _ = helpers.request(connection, method, f"/session/{session_id}/{suffix}", payload)
        assert status == 200, result
        return result["value"]

    def record_keys():
        bidi.evaluate(
            parent,
            """window.navigationInputEvents = [];
            for (const type of ['keydown', 'keyup']) {
                document.addEventListener(type, event => navigationInputEvents.push([
                    event.type, event.key, event.shiftKey, event.repeat
                ]));
            }
            document.getElementById('target').focus();""",
        )

    def events():
        return json.loads(bidi.evaluate(parent, "JSON.stringify(navigationInputEvents)")["result"]["value"])

    actions = [{"type": "key", "id": "navigation-keyboard", "actions": [{"type": "keyDown", "value": "g"}]}]
    bidi.command("browsingContext.activate", {"context": parent})
    record_keys()
    bidi.command("input.performActions", {"context": parent, "actions": actions})
    assert events() == [["keydown", "g", False, False]], events()

    # The localhost OOP child navigates the 127.0.0.1 top-level context to its own site.
    classic("POST", "window", {"handle": parent})
    classic("POST", "frame", {"id": None})
    frame = classic("POST", "element", {"using": "css selector", "value": "#input-child"})
    classic("POST", "frame", {"id": frame})
    bidi.evaluate(parent, "document.getElementById('input-child').focus(); 0")
    bidi.evaluate(child, "document.getElementById('target').focus(); 0")
    link = classic("POST", "element", {"using": "css selector", "value": "#navigate-top"})
    subscription = bidi.command("session.subscribe", {"events": ["browsingContext.load"], "contexts": [parent]})[
        "subscription"
    ]
    classic("POST", f"element/{link['element-6066-11e4-a52e-4f735466cecf']}/click", {})
    # The click succeeds and its actual cross-site navigation completes before checking the retained input state.
    while True:
        loaded = bidi.event("browsingContext.load")
        if loaded["context"] == parent and loaded["url"] == replacement_url:
            break
    assert isinstance(loaded["navigation"], str), loaded
    bidi.command("session.unsubscribe", {"subscriptions": [subscription]})
    result = bidi.evaluate(parent, "document.URL")
    assert result["result"]["value"] == replacement_url, result
    tree = bidi.command("browsingContext.getTree", {"root": parent})["contexts"][0]
    assert tree["context"] == parent and tree["children"] == [], tree

    record_keys()
    bidi.command("input.releaseActions", {"context": parent})
    assert events() == [["keyup", "g", False, False]], events()
    bidi.command("input.performActions", {"context": parent, "actions": actions})
    assert events()[-1] == ["keydown", "g", False, False], events()

    # The old child no longer exists. Only the stable top-level ID participates in success cleanup;
    # failures use the enclosing session cleanup, which preserves the original error.
    classic("POST", "window", {"handle": parent})
    classic("POST", "frame", {"id": None})
    bidi.command("input.releaseActions", {"context": parent})
    bidi.command("browsingContext.navigate", {"context": parent, "url": "about:blank", "wait": "complete"})


def check_screenshot_lifecycle(bidi, context, cases=("hidden", "hide", "navigate")):
    def wait_visibility(target, visible):
        expected_hidden = "false" if visible else "true"
        result = bidi.evaluate(
            target,
            "new Promise(resolve => { const changed = () => { if (document.hidden === "
            + expected_hidden
            + ") { document.removeEventListener('visibilitychange', changed); resolve(document.hidden); } }; document.addEventListener('visibilitychange', changed); changed(); })",
            awaitPromise=True,
        )
        assert result["result"]["value"] is (not visible), result
        bidi.evaluate(target, "0")

    def expect_canceled(command_id):
        # The same-context evaluation is an IPC barrier: cancellation must precede its response, without GC.
        assert command_id in bidi.responses, "Screenshot remained pending after its document became unavailable"
        result = bidi.response(command_id)
        assert result["type"] == "error" and result["error"] == "unsupported operation", result
        bidi.evaluate(context, "internals.gc(); 0")
        assert command_id not in bidi.responses, "Screenshot completed twice after collection"

    bidi.command("browsingContext.activate", {"context": context})
    other = bidi.command("browsingContext.create", {"type": "tab", "background": True})["context"]
    wait_visibility(context, True)
    try:
        if "hidden" in cases:
            wait_visibility(other, False)
            hidden = bidi.send("browsingContext.captureScreenshot", {"context": other})
            bidi.evaluate(other, "0")
            expect_canceled(hidden)

        if "hide" in cases:
            # Hold frame delivery so hiding a visible document must cancel its queued capture directly.
            bidi.evaluate(context, "internals.setManualRenderingOpportunities(true)")
            pending = bidi.send("browsingContext.captureScreenshot", {"context": context})
            bidi.evaluate(context, "0")
            assert pending not in bidi.responses, bidi.responses
            bidi.command("browsingContext.activate", {"context": other})
            wait_visibility(context, False)
            expect_canceled(pending)
            bidi.command("browsingContext.activate", {"context": context})
            wait_visibility(context, True)
            bidi.evaluate(context, "internals.setManualRenderingOpportunities(false)")

        if "navigate" in cases:
            # The parent retains the old child document and its window throughout navigation.
            bidi.evaluate(
                context,
                "new Promise(resolve => { const f = document.createElement('iframe'); f.onload = () => { if (f.contentDocument.URL !== 'about:blank?capture-child') return; f.onload = null; window.retainedCaptureDocument = f.contentDocument; window.retainedCaptureFunction = f.contentWindow.Function('return document'); resolve(); }; f.src = 'about:blank?capture-child'; document.body.append(f); })",
                awaitPromise=True,
            )
            child = bidi.command("browsingContext.getTree", {"root": context})["contexts"][0]["children"][0]["context"]
            result = bidi.evaluate(child, "internals.setManualRenderingOpportunities(true); document.URL")
            assert result["result"]["value"] == "about:blank?capture-child", result
            pending = bidi.send("browsingContext.captureScreenshot", {"context": child})
            bidi.evaluate(child, "0")
            assert pending not in bidi.responses, bidi.responses
            bidi.command(
                "browsingContext.navigate", {"context": child, "url": "about:blank?after-capture", "wait": "complete"}
            )
            assert (
                bidi.evaluate(
                    context,
                    "retainedCaptureFunction() === retainedCaptureDocument && retainedCaptureDocument !== document.querySelector('iframe').contentDocument",
                )["result"]["value"]
                is True
            )
            expect_canceled(pending)
    finally:
        bidi.command("browsingContext.activate", {"context": context})
        wait_visibility(context, True)
        bidi.evaluate(context, "internals.setManualRenderingOpportunities(false)")
        bidi.command("browsingContext.close", {"context": other})
        bidi.command("browsingContext.navigate", {"context": context, "url": "about:blank", "wait": "complete"})


def check_screenshot_frame_order(bidi, context):
    bidi.command("browsingContext.activate", {"context": context})
    bidi.evaluate(
        context,
        "document.documentElement.style.background = 'lime'; document.body.style.cssText = 'margin:0;height:100vh;background:lime'; internals.setManualRenderingOpportunities(true)",
    )

    def capture(script):
        pending = bidi.send(
            "browsingContext.captureScreenshot", {"context": context, "imageSize": {"maxWidth": 1, "maxHeight": 1}}
        )
        bidi.evaluate(context, script + "; internals.injectRenderingOpportunity(performance.now() + 16); 0")
        result = bidi.response(pending)
        assert result["type"] == "success", result
        return result["result"]["data"]

    try:
        green = capture("0")
        assert capture("0") == green, "Identical-color reference screenshots differ"
        bidi.evaluate(
            context, "document.documentElement.style.background = 'red'; document.body.style.background = 'red'"
        )
        actual = capture(
            "requestAnimationFrame(() => { document.documentElement.style.background = 'lime'; document.body.style.background = 'lime'; })"
        )
        assert actual == green, f"Capture ran before a later callback in the same frame: actual={actual}, green={green}"
    finally:
        bidi.evaluate(context, "internals.setManualRenderingOpportunities(false)")
        bidi.command("browsingContext.navigate", {"context": context, "url": "about:blank", "wait": "complete"})


def check_viewport_changes(bidi, connection, session_id, first, second):
    def metrics(context):
        result = bidi.evaluate(context, "[innerWidth, innerHeight, devicePixelRatio]")
        return [item["value"] for item in result["result"]["value"]]

    bidi.command(
        "browsingContext.setViewport",
        {"context": first, "viewport": {"width": 320, "height": 200}, "devicePixelRatio": 1},
    )
    bidi.command(
        "browsingContext.setViewport",
        {"context": second, "viewport": {"width": 320, "height": 200}, "devicePixelRatio": 2},
    )
    # Keep the requested CSS size across fractional device ratios, including after a failed batch.
    bidi.command(
        "browsingContext.setViewport",
        {"context": first, "viewport": {"width": 333, "height": 201}, "devicePixelRatio": 1.5},
    )
    assert metrics(first) == [333, 201, 1.5], metrics(first)
    bidi.command("browsingContext.setViewport", {"context": first, "devicePixelRatio": 0.5})
    before = [metrics(first), metrics(second)]
    result = bidi.response(
        bidi.send(
            "browsingContext.setViewport", {"userContexts": ["default"], "viewport": {"width": 9000, "height": 1}}
        )
    )
    assert result["type"] == "error" and result["error"] == "unsupported operation", result
    after = [metrics(first), metrics(second)]
    assert after == before, f"Failed viewport command changed targets: before={before}, after={after}"

    bidi.command("browsingContext.setViewport", {"context": first, "devicePixelRatio": 2})
    assert metrics(first) == [333, 201, 2], metrics(first)
    bidi.command("browsingContext.setViewport", {"context": first, "devicePixelRatio": 1.25})
    assert metrics(first) == [333, 201, 1.25], metrics(first)
    # Integer device pixels quantize this viewport at DPR < 1; retain its exact requested CSS size anyway.
    bidi.command("browsingContext.setViewport", {"context": first, "devicePixelRatio": 0.5})
    assert metrics(first) == [334, 202, 0.5], metrics(first)
    bidi.command("browsingContext.setViewport", {"context": first, "devicePixelRatio": 2})
    assert metrics(first) == [333, 201, 2], metrics(first)

    # A rejected first override must not capture the defaults used by a later successful override.
    fresh = bidi.command("browsingContext.create", {"type": "window"})["context"]
    result = bidi.response(
        bidi.send(
            "browsingContext.setViewport",
            {"context": fresh, "viewport": {"width": 16385, "height": 1}, "devicePixelRatio": 1},
        )
    )
    assert result["type"] == "error" and result["error"] == "unsupported operation", result
    status, payload, _ = helpers.request(connection, "POST", f"/session/{session_id}/window", {"handle": fresh})
    assert status == 200, payload
    status, payload, _ = helpers.request(
        connection, "POST", f"/session/{session_id}/window/rect", {"width": 640, "height": 480}
    )
    assert status == 200, payload
    expected_default = metrics(fresh)
    assert expected_default[:2] == [640, 480], expected_default
    bidi.command(
        "browsingContext.setViewport",
        {"context": fresh, "viewport": {"width": 200, "height": 100}, "devicePixelRatio": 1},
    )
    bidi.command("browsingContext.setViewport", {"context": fresh, "viewport": None, "devicePixelRatio": None})
    restored = metrics(fresh)
    assert restored == expected_default, (
        f"Failed command captured stale defaults: expected={expected_default}, actual={restored}"
    )
    # A classic window resize supersedes the CSS size from the previous BiDi override.
    bidi.command(
        "browsingContext.setViewport",
        {"context": fresh, "viewport": {"width": 333, "height": 201}, "devicePixelRatio": 1.5},
    )
    status, payload, _ = helpers.request(
        connection, "POST", f"/session/{session_id}/window/rect", {"width": 600, "height": 450}
    )
    assert status == 200, payload
    assert metrics(fresh) == [400, 300, 1.5], metrics(fresh)
    bidi.command("browsingContext.setViewport", {"context": fresh, "devicePixelRatio": 2})
    assert metrics(fresh) == [400, 300, 2], metrics(fresh)
    bidi.command("browsingContext.setViewport", {"context": fresh, "viewport": None, "devicePixelRatio": None})
    assert metrics(fresh) == expected_default, metrics(fresh)
    helpers.request(connection, "POST", f"/session/{session_id}/window", {"handle": first})
    bidi.command("browsingContext.close", {"context": fresh})
    for context in [first, second]:
        bidi.command("browsingContext.setViewport", {"context": context, "viewport": None, "devicePixelRatio": None})


def cleanup_resources(server, webdriver, connection, bidi, session_id, *, primary_error=None):
    errors = []

    def attempt(action):
        try:
            action()
        except BaseException as error:
            errors.append(error)

    def stop_driver():
        if webdriver is None:
            return
        attempt(webdriver.terminate)
        try:
            webdriver.wait(timeout=5)
        except BaseException as error:
            if not isinstance(error, subprocess.TimeoutExpired):
                errors.append(error)
            attempt(webdriver.kill)
            attempt(lambda: webdriver.wait(timeout=5))

    attempt(server.release_response.set)
    attempt(server.release_interception.set)
    if session_id and connection:
        attempt(lambda: helpers.request(connection, "DELETE", f"/session/{session_id}"))
    if bidi:
        attempt(bidi.close)
    if connection:
        attempt(connection.close)
    attempt(stop_driver)
    attempt(server.shutdown)
    attempt(server.server_close)
    if errors and primary_error is None:
        raise errors[0]


def run_test(
    webdriver_binary,
    *,
    input_cross_context_only=False,
    network_download_only=False,
    network_interception_only=False,
    network_interception_case="all",
):
    with tempfile.TemporaryDirectory(prefix="ladybird-webdriver-bidi-") as profile:
        downloads = os.path.join(profile, "downloads")
        os.mkdir(downloads)
        server = ReloadServer()
        threading.Thread(target=server.serve_forever, daemon=True).start()
        port = helpers.unused_port()
        webdriver = None
        connection = None
        bidi = None
        session_id = None
        try:
            webdriver = subprocess.Popen(
                [
                    webdriver_binary,
                    "--headless",
                    "--expose-internals-object",
                    "--site-isolation=iframe",
                    f"--profile-path={profile}",
                    "-l",
                    "127.0.0.1",
                    "-p",
                    str(port),
                ],
                env={**os.environ, "XDG_DOWNLOAD_DIR": downloads},
            )
            helpers.wait_for_port(port)
            connection = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
            status, payload, _ = helpers.request(
                connection,
                "POST",
                "/session",
                {"capabilities": {"alwaysMatch": {"webSocketUrl": True}}},
            )
            assert status == 200, payload
            session_id = payload["value"]["sessionId"]
            websocket_url = payload["value"]["capabilities"]["webSocketUrl"]
            # The transport is reusable; WebDriver still owns the loopback Host, Origin and session-path policy.
            for policy in [
                {"host": f"example.test:{urllib.parse.urlsplit(websocket_url).port}"},
                {"origin": "https://example.test"},
            ]:
                rejected = BiDiConnection(websocket_url, expected_status=403, **policy)
                rejected.close()
            unknown_session_url = websocket_url.rsplit("/", 1)[0] + "/00000000-0000-4000-8000-000000000000"
            rejected = BiDiConnection(unknown_session_url, expected_status=404)
            rejected.close()
            bidi = BiDiConnection(websocket_url)
            context = bidi.command("browsingContext.getTree", {})["contexts"][0]["context"]
            if network_interception_only:
                check_network_interception(bidi, context, server, network_interception_case)
                status, payload, _ = helpers.request(connection, "DELETE", f"/session/{session_id}")
                assert status == 200, payload
                session_id = None
                return
            if network_download_only:
                check_network_download_events(bidi, context, server, downloads)
                status, payload, _ = helpers.request(connection, "DELETE", f"/session/{session_id}")
                assert status == 200, payload
                session_id = None
                return
            if input_cross_context_only:
                check_input_cross_context(bidi, connection, session_id, context, server)
                check_input_click_navigation(bidi, connection, session_id, context, server)
                status, payload, _ = helpers.request(connection, "DELETE", f"/session/{session_id}")
                assert status == 200, payload
                session_id = None
                return
            check_network_interception(bidi, context, server, network_interception_case)
            check_network_download_events(bidi, context, server, downloads)
            check_network_adopted_navigation_events(bidi, context, server)
            check_network_abort_before_transport(bidi, context, server)
            check_screenshot_handles(bidi, context)
            check_input_handles(bidi, context)
            check_input_queue(bidi, context)
            check_input_cross_context(bidi, connection, session_id, context, server)
            check_input_click_navigation(bidi, connection, session_id, context, server)
            check_screenshot_lifecycle(bidi, context)
            check_screenshot_frame_order(bidi, context)

            bidi.command("browsingContext.getTree", {"maxDepth": 1.0})
            for depth in [-1, 0.5, float(2**53)]:
                result = bidi.response(bidi.send("browsingContext.getTree", {"maxDepth": depth}))
                assert result["type"] == "error" and result["error"] == "invalid argument", result

            # Object classification uses the engine's public embedding API.
            values = {
                "new WeakMap()": "weakmap",
                "new WeakSet()": "weakset",
                "(function*() {})()": "generator",
                "(async function*() {})()": "generator",
                "new Proxy({}, {})": "proxy",
                "new ArrayBuffer(8)": "arraybuffer",
                "new Map([[1, 2]])": "map",
                "new Set([1, 2])": "set",
            }
            for expression, expected_type in values.items():
                result = bidi.evaluate(context, expression)
                assert result["type"] == "success", result
                assert result["result"]["type"] == expected_type, result

            # Deserialization creates values in the selected environment and wraps shared nodes there.
            node = bidi.evaluate(context, "document.body")["result"]
            result = bidi.command(
                "script.callFunction",
                {
                    "target": {"context": context},
                    "functionDeclaration": "(array, date, map, set, object, node) => [array[0], date.getUTCFullYear(), map.get(1), set.has(2), object.k, node === document.body]",
                    "awaitPromise": False,
                    "arguments": [
                        {"type": "array", "value": [{"type": "number", "value": 7}]},
                        {"type": "date", "value": "2026-01-01T00:00:00.000Z"},
                        {"type": "map", "value": [[{"type": "number", "value": 1}, {"type": "number", "value": 2}]]},
                        {"type": "set", "value": [{"type": "number", "value": 2}]},
                        {"type": "object", "value": [["k", {"type": "number", "value": 9}]]},
                        {"sharedId": node["sharedId"]},
                    ],
                },
            )
            assert [value["value"] for value in result["result"]["value"]] == [7, 2026, 2, True, 9, True], result
            environment = bidi.command("script.getRealms", {"context": context})["realms"][0]["realm"]
            result = bidi.command(
                "script.evaluate", {"target": {"realm": environment}, "expression": "42", "awaitPromise": False}
            )
            assert result["result"]["value"] == 42, result

            bidi.command("session.subscribe", {"events": ["script.message", "log.entryAdded"]})
            bidi.command(
                "script.callFunction",
                {
                    "target": {"context": context},
                    "functionDeclaration": "channel => { channel(17); }",
                    "awaitPromise": False,
                    "arguments": [{"type": "channel", "value": {"channel": "test"}}],
                },
            )
            message = bidi.event("script.message")
            assert message["channel"] == "test" and message["data"]["value"] == 17, message
            bidi.evaluate(context, "console.log('BiDi test')")
            message = bidi.event("log.entryAdded")
            assert message["text"] == "BiDi test" and message["source"]["realm"] == environment, message

            # The pending response must survive collection even if script keeps no reference to its promise.
            pending = bidi.send(
                "script.evaluate",
                {"target": {"context": context}, "expression": "new Promise(() => {})", "awaitPromise": True},
            )
            bidi.evaluate(context, "internals.gc(); undefined")
            assert pending not in bidi.responses, bidi.responses

            # Another awaited promise settles normally and releases its retained state.
            settling = bidi.send(
                "script.evaluate",
                {
                    "target": {"context": context},
                    "expression": "new Promise(resolve => { window.finishBiDi = resolve; })",
                    "awaitPromise": True,
                },
            )
            bidi.evaluate(context, "finishBiDi(42)")
            result = bidi.response(settling)
            assert result["type"] == "success" and result["result"]["result"]["value"] == 42, result

            # Destroying the original environment completes the unreachable promise outside garbage collection.
            bidi.command(
                "browsingContext.navigate", {"context": context, "url": "about:blank?next", "wait": "complete"}
            )
            result = bidi.response(pending)
            assert result["type"] == "error" and result["error"] == "unknown error", result

            # Preloads run once in each real document, including child documents.
            bidi.command(
                "script.addPreloadScript",
                {"functionDeclaration": "() => { window.preloadCount = (window.preloadCount ?? 0) + 1; }"},
            )
            bidi.command(
                "browsingContext.navigate", {"context": context, "url": "about:blank?preload", "wait": "complete"}
            )
            assert bidi.evaluate(context, "preloadCount")["result"]["value"] == 1
            created = bidi.command("browsingContext.create", {"type": "tab"})["context"]
            assert bidi.evaluate(created, "preloadCount")["result"]["value"] == 1
            result = bidi.evaluate(
                context,
                "new Promise(resolve => { const frame = document.createElement('iframe'); frame.onload = () => resolve(frame.contentWindow.preloadCount); frame.src = 'about:blank?child'; document.body.append(frame); })",
                awaitPromise=True,
            )
            assert result["result"]["value"] == 1, result

            check_viewport_changes(bidi, connection, session_id, context, created)

            bidi.command(
                "session.subscribe",
                {"events": ["browsingContext.navigationStarted", "browsingContext.navigationAborted"]},
            )
            # A top-level reload answers with the navigation ID announced before its document loads.
            for wait in ["none", "interactive", "complete"]:
                result = bidi.command("browsingContext.reload", {"context": context, "wait": wait})
                started = bidi.event("browsingContext.navigationStarted")
                assert result["navigation"] == started["navigation"] and result["navigation"] is not None, result
            assert bidi.evaluate(context, "preloadCount")["result"]["value"] == 1

            # Replacing a reload that is still waiting for its response cancels the old waiter.
            reload_url = f"http://127.0.0.1:{server.server_port}/reload"
            bidi.command("browsingContext.navigate", {"context": context, "url": reload_url, "wait": "complete"})
            # Consume the navigate event before checking the two reloads.
            bidi.event("browsingContext.navigationStarted")
            server.block_reload = True
            previous_reload = bidi.send("browsingContext.reload", {"context": context, "wait": "complete"})
            assert server.request_received.wait(30), "The reload did not request its document"
            previous_started = bidi.event("browsingContext.navigationStarted")
            replacement_reload = bidi.send("browsingContext.reload", {"context": context, "wait": "complete"})
            result = bidi.response(previous_reload)
            assert result["type"] == "error" and result["error"] == "unknown error", result
            # A command in the still-active document is an event-loop barrier while both responses are held.
            bidi.evaluate(context, "document.URL")
            assert replacement_reload not in bidi.responses, bidi.responses
            server.release_response.set()
            replacement_started = bidi.event("browsingContext.navigationStarted")
            assert replacement_started["navigation"] != previous_started["navigation"], replacement_started
            result = bidi.response(replacement_reload)
            assert result["type"] == "success", result
            assert result["result"] == {"navigation": replacement_started["navigation"], "url": reload_url}, result

            # A different navigation cannot supply the held reload's result or navigation ID.
            server.request_received.clear()
            server.release_response.clear()
            held_reload = bidi.send("browsingContext.reload", {"context": context, "wait": "complete"})
            held_started = bidi.event("browsingContext.navigationStarted")
            assert server.request_received.wait(30), "The reload did not request its document"
            other_navigation = bidi.send(
                "browsingContext.navigate", {"context": context, "url": "about:blank?replacement", "wait": "complete"}
            )
            other_started = bidi.event("browsingContext.navigationStarted")
            assert held_started["navigation"] != other_started["navigation"], other_started
            bidi.evaluate(context, "document.URL")
            if held_reload in bidi.responses:
                assert bidi.responses[held_reload]["type"] == "error", bidi.responses[held_reload]
            server.release_response.set()
            held_result = bidi.response(held_reload)
            assert held_result["type"] == "error" and held_result["error"] == "unknown error", held_result
            other_result = bidi.response(other_navigation)
            assert other_result["type"] == "success", other_result
            assert other_result["result"]["navigation"] == other_started["navigation"], other_result

            # Ending a session also releases an outstanding evaluation and its promise.
            bidi.send(
                "script.evaluate",
                {"target": {"context": context}, "expression": "new Promise(() => {})", "awaitPromise": True},
            )
            bidi.evaluate(context, "internals.gc(); undefined")
            status, payload, _ = helpers.request(connection, "DELETE", f"/session/{session_id}")
            assert status == 200, payload
            session_id = None
        finally:
            cleanup_resources(server, webdriver, connection, bidi, session_id, primary_error=sys.exc_info()[1])


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument("--input-cross-context-only", action="store_true")
    parser.add_argument("--network-download-only", action="store_true")
    parser.add_argument("--network-interception-only", action="store_true")
    parser.add_argument(
        "--network-interception-case",
        choices=[
            "all",
            "late-enable",
            "late-abort",
            "redirect",
            "auth",
            "overrides",
            "detached",
            "navigation",
            "scoped-headers",
            "fail-transport",
            "provided-body-at-response-started",
            "document-abort-object",
            "abort-after-gc",
            "events-off-abort",
            "events-off-stop",
            "events-on-stop",
            "continue-after-gc",
            "adopted-navigation-continue",
            "adopted-navigation-fail",
            "adopted-navigation-provide",
            "auth-gates",
            "auth-omit",
            "auth-cors",
            "auth-stream",
            "auth-cancel",
            "auth-default",
            "echo-headers",
            "echo-307",
            "echo-303",
            "echo-auth",
            "explicit-307",
            "explicit-303",
            "explicit-auth",
            "echo-body",
            "echo-range",
            "explicit-range",
            "explicit-unsubscribe",
            "auth-response-matrix",
            "auth-action-extras",
            "auth-cancel-extra",
            "auth-default-extra",
            "auth-provide-response",
            "auth-provide-response-omitted",
            "auth-provide-response-401",
            "auth-provide-response-200",
            "auth-continue-response",
            "auth-continue-response-default",
            "auth-continue-response-credentials",
        ],
        default="all",
    )
    args = parser.parse_args()
    run_test(
        args.webdriver_binary,
        input_cross_context_only=args.input_cross_context_only,
        network_download_only=args.network_download_only,
        network_interception_only=args.network_interception_only,
        network_interception_case=args.network_interception_case,
    )
