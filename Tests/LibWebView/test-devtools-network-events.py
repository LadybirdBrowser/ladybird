#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import tempfile
import threading

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
TIMEOUT_SECONDS = devtools["TIMEOUT_SECONDS"]

TITLE = "network-events"
BODY = b"DevTools network event body"


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.held_transport_closed = threading.Event()
        self.misdirected_preflight_sent = False
        self.release_misdirected_body = threading.Event()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        server = self.server
        assert isinstance(server, Server)
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.path == "/page":
            self.send_body("text/html", f"<!doctype html><title>{TITLE}</title>".encode())
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/redirected")
            self.send_header("Content-Length", "0")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        if self.path == "/held":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BODY) * 1000))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(BODY)
            self.wfile.flush()
            self.connection.settimeout(TIMEOUT_SECONDS)
            try:
                if not self.connection.recv(1):
                    server.held_transport_closed.set()
            except (ConnectionResetError, BrokenPipeError):
                server.held_transport_closed.set()
            return
        if self.path == "/trailers":
            self.protocol_version = "HTTP/1.1"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.send_header("Trailer", "X-Trailer")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"%x\r\n%s\r\n0\r\nX-Trailer: trailer\r\n\r\n" % (len(BODY), BODY))
            return
        self.send_body("text/plain", BODY, allow_cross_origin=self.path.startswith("/preflight-"))

    def do_OPTIONS(self):
        server = self.server
        assert isinstance(server, Server)
        if self.path == "/preflight-fails":
            self.send_body("text/plain", b"")
            return
        if self.path == "/preflight-misdirected" and not server.misdirected_preflight_sent:
            server.misdirected_preflight_sent = True
            self.send_response(421)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(BODY) * 2))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(BODY)
                self.wfile.flush()
                if server.release_misdirected_body.wait(TIMEOUT_SECONDS):
                    self.wfile.write(BODY)
            except (ConnectionResetError, BrokenPipeError):
                pass
            return
        self.send_body("text/plain", b"", allow_cross_origin=True)

    def send_body(self, content_type, body, allow_cross_origin=False):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if allow_cross_origin:
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "X-Test")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


class NetworkEventClient(devtools["DevToolsClient"]):
    # Keeps every network event and update the client receives.
    def __init__(self, port):
        self.network_events = {}
        self.network_event_updates = {}
        super().__init__(port)

    def receive(self):
        packet = super().receive()
        if packet.get("type") in ["resources-available-array", "resources-updated-array"]:
            for resource_type, resources in packet["array"]:
                if resource_type != "network-event":
                    continue
                for resource in resources:
                    resource_id = resource["resourceId"]
                    if packet["type"] == "resources-available-array":
                        self.network_events[resource_id] = resource
                    else:
                        self.network_event_updates.setdefault(resource_id, []).append(resource["resourceUpdates"])
        return packet

    def network_events_for(self, url, method=None):
        return [
            resource_id
            for resource_id, event in self.network_events.items()
            if event["url"] == url and (method is None or event["method"] == method)
        ]

    def network_event_methods_for(self, url):
        return [self.network_events[resource_id]["method"] for resource_id in self.network_events_for(url)]

    def wait_for_update(self, url, flag, method=None):
        def has_update():
            resource_ids = self.network_events_for(url, method)
            updates = self.network_event_updates.get(resource_ids[-1], []) if resource_ids else []
            return any(update.get(flag) for update in updates)

        while not has_update():
            self.receive_matching(lambda packet: True)
        return self.network_events_for(url, method)[-1]

    def wait_for_resource_update(self, resource_id, flag):
        while not any(update.get(flag) for update in self.network_event_updates.get(resource_id, [])):
            self.receive_matching(lambda packet: True)


def check_fetch(client, origin):
    url = f"{origin}/body"
    client.evaluate("fetch('/body').then(response => response.text()); 0")
    resource_id = client.wait_for_update(url, "responseContentAvailable")
    assert client.network_events_for(url) == [resource_id], client.network_events
    event = client.network_events[resource_id]
    assert event["method"] == "GET" and event["isXHR"] is True and event["cause"] == {"type": "fetch"}, event
    updates = client.network_event_updates[resource_id]
    assert len(updates) == 2, updates
    headers, completion = updates
    assert headers["responseHeadersAvailable"] and headers["status"] == "200", headers
    assert headers["mimeType"] == "text/plain", headers
    assert completion["responseContentAvailable"] and completion["contentSize"] == len(BODY), completion
    client.send({"to": event["actor"], "type": "getResponseContent"})
    content = client.receive_matching(lambda packet: packet.get("from") == event["actor"])["content"]
    assert content["text"] == BODY.decode(), content
    print("PASS: A fetch reports a network event, an update for its headers, one for its completion, and its body")


def check_redirect(client, origin):
    url = f"{origin}/redirect"
    final_url = f"{origin}/redirected"
    client.evaluate("fetch('/redirect').then(response => response.text()); 0")
    final_id = client.wait_for_update(final_url, "responseContentAvailable")
    requests = [event["url"] for event in client.network_events.values() if event["url"] in [url, final_url]]
    assert requests == [url, final_url], client.network_events
    # NB: The redirect's own request reports a completion only if its transport finishes before the fetch stops it.
    headers = client.network_event_updates[client.network_events_for(url)[0]][0]
    assert headers["responseHeadersAvailable"] and headers["status"] == "302", headers
    updates = client.network_event_updates[final_id]
    assert len(updates) == 2, updates
    headers, completion = updates
    assert headers["responseHeadersAvailable"] and headers["status"] == "200", headers
    assert completion["responseContentAvailable"] and completion["contentSize"] == len(BODY), completion
    print("PASS: A redirected fetch reports a network event for each request")


def check_trailers(client, origin):
    url = f"{origin}/trailers"
    client.evaluate("fetch('/trailers').then(response => response.text()); 0")
    resource_id = client.wait_for_update(url, "responseContentAvailable")
    updates = client.network_event_updates[resource_id]
    assert len(updates) == 2, updates
    headers, completion = updates
    assert headers["responseHeadersAvailable"] and headers["status"] == "200", headers
    assert completion["responseContentAvailable"] and completion["contentSize"] == len(BODY), completion
    print("PASS: A response with a trailer reports an update for its headers and one for its completion")


def check_data_url(client, origin):
    client.evaluate(
        "fetch('data:,network-event').then(response => response.text())"
        ".then(() => fetch('/after-data')).then(response => response.text()); 0"
    )
    client.wait_for_update(f"{origin}/after-data", "responseContentAvailable")
    assert not any(event["url"].startswith("data:") for event in client.network_events.values()), client.network_events
    print("PASS: A data: URL fetch reports no network event")


def preflighted_fetch(url):
    return f"fetch({json.dumps(url)}, {json.dumps({'headers': {'X-Test': 'preflight'}})})"


def assert_preflight_headers(client, resource_id):
    # NB: A preflight's request reports a completion only if its transport finishes before the preflight stops it.
    headers = client.network_event_updates[resource_id][0]
    assert headers["responseHeadersAvailable"] and headers["status"] == "200", headers


def check_preflight(client, cross_origin):
    url = f"{cross_origin}/preflight-passes"
    client.evaluate(f"{preflighted_fetch(url)}.then(response => response.text()); 0")
    client.wait_for_update(url, "responseContentAvailable", "GET")
    assert client.network_event_methods_for(url) == ["OPTIONS", "GET"], client.network_events
    preflight_id, fetch_id = client.network_events_for(url)
    assert_preflight_headers(client, preflight_id)
    updates = client.network_event_updates[fetch_id]
    assert len(updates) == 2, updates
    headers, completion = updates
    assert headers["responseHeadersAvailable"] and headers["status"] == "200", headers
    assert completion["responseContentAvailable"] and completion["contentSize"] == len(BODY), completion
    print("PASS: A preflighted fetch reports a network event for its preflight's request and then one for its own")


def check_failed_preflight(client, cross_origin):
    url = f"{cross_origin}/preflight-fails"
    client.evaluate(f"{preflighted_fetch(url)}.catch(error => window.failedPreflight = error.name); 0")
    devtools["wait_until"]("the fetch fails", lambda: client.evaluate("window.failedPreflight") == "TypeError")
    assert client.network_event_methods_for(url) == ["OPTIONS"], client.network_events
    assert_preflight_headers(client, client.network_events_for(url)[0])
    print("PASS: A fetch whose preflight fails reports a network event for its preflight's request alone")


def check_misdirected_preflight(client, cross_origin, server):
    url = f"{cross_origin}/preflight-misdirected"
    client.evaluate(
        f"{preflighted_fetch(url)}.then(response => response.text()).then(text => window.preflightedText = text); 0"
    )
    devtools["wait_until"]("the fetch ends", lambda: client.evaluate("window.preflightedText") == BODY.decode())
    assert client.network_event_methods_for(url) == ["OPTIONS", "OPTIONS", "GET"], client.network_events
    misdirected_id, retried_id, _ = client.network_events_for(url)
    updates = client.network_event_updates[misdirected_id]
    assert len(updates) == 1 and updates[0]["responseHeadersAvailable"] and updates[0]["status"] == "421", updates
    assert_preflight_headers(client, retried_id)
    client.evaluate("internals.gc(); 0")
    server.release_misdirected_body.set()
    client.wait_for_resource_update(misdirected_id, "responseContentAvailable")
    completion = client.network_event_updates[misdirected_id][1]
    assert completion["contentSize"] == len(BODY) * 2, completion
    assert client.evaluate("window.preflightedText") == BODY.decode(), "The page lost the fetch's result"
    print("PASS: A preflight's request that got a 421 reports its completion after its fetch ends and a collection")


def check_stopped_fetch(client, origin, server):
    url = f"{origin}/held"
    client.evaluate(
        "window.heldFetch = new AbortController(); "
        "fetch('/held', {signal: heldFetch.signal}).then(response => response.text()).catch(() => {}); 0"
    )
    resource_id = client.wait_for_update(url, "responseHeadersAvailable")
    client.evaluate("heldFetch.abort(); 0")
    assert server.held_transport_closed.wait(TIMEOUT_SECONDS), "The stopped fetch left its transport open"
    # The fetch stops its request before the transport closes, so the next reply follows any update the stop sent.
    client.evaluate("0")
    updates = client.network_event_updates[resource_id]
    assert len(updates) == 1 and updates[0]["responseHeadersAvailable"], updates
    print("PASS: A fetch stopped while its body is held reports no completion")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    server = Server()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    origin = f"http://127.0.0.1:{server.server_port}"
    cross_origin = f"http://localhost:{server.server_port}"
    url = f"{origin}/page"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-devtools-") as profile:
            process, devtools_port, client = devtools["launch"](ladybird_binary, profile, url)
            try:
                try:
                    devtools["wait_for_page"](client, url, TITLE)
                finally:
                    client.close()
                client = NetworkEventClient(devtools_port)
                try:
                    client.watch_frame_targets()
                    devtools["wait_until"](
                        "the page loads", lambda: client.evaluate("document.readyState") == "complete"
                    )
                    # A load that ended after the target was created replaces it before the next request is handled.
                    client.evaluate("0")
                    check_fetch(client, origin)
                    check_redirect(client, origin)
                    check_trailers(client, origin)
                    check_data_url(client, origin)
                    check_preflight(client, cross_origin)
                    check_failed_preflight(client, cross_origin)
                    check_misdirected_preflight(client, cross_origin, server)
                    check_stopped_fetch(client, origin, server)
                finally:
                    client.close()
                assert process.poll() is None, "The browser exited"
            finally:
                devtools["stop"](process)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
