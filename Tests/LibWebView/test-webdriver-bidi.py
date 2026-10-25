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


class ReloadHandler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        server = self.server
        assert isinstance(server, ReloadServer)
        if server.block_reload:
            server.request_received.set()
            if not server.release_response.wait(30):
                raise TimeoutError("The test did not release the reload response")
        body = b"<!doctype html><title>Reload target</title>"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except BrokenPipeError:
            pass

    def log_message(self, format, *args):
        pass


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


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-webdriver-bidi-") as profile:
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
                    f"--profile-path={profile}",
                    "-l",
                    "127.0.0.1",
                    "-p",
                    str(port),
                ]
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
    run_test(parser.parse_args().webdriver_binary)
