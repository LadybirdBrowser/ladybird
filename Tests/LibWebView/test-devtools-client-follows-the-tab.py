#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import socket
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))
TIMEOUT_SECONDS = helpers["EVENT_TIMEOUT_SECONDS"]


loads_lock = threading.Lock()
loads = {}


def load_count(path):
    with loads_lock:
        return loads.get(path, 0)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        with loads_lock:
            load = loads.get(self.path, 0) + 1
            loads[self.path] = load
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(f"<!doctype html><title>{self.path}</title><script>window.load = {load};</script>".encode())

    def log_message(self, format, *args):
        pass


class ConnectionClosed(Exception):
    pass


class DevToolsClient:
    def __init__(self, port, deadline=None):
        # The server takes one client at a time, and turns another away until it's done with the previous one.
        if deadline is None:
            deadline = time.monotonic() + TIMEOUT_SECONDS
        while True:
            try:
                self.socket = socket.create_connection(("127.0.0.1", port), timeout=TIMEOUT_SECONDS)
            except OSError:
                # The server isn't listening yet.
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)
                continue
            self.buffer = b""
            self.console = None
            try:
                self.receive()
                return
            except ConnectionClosed:
                self.socket.close()
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.05)

    def send(self, packet):
        data = json.dumps(packet).encode()
        self.socket.sendall(str(len(data)).encode() + b":" + data)

    def receive(self):
        while True:
            separator = self.buffer.find(b":")
            if separator != -1:
                length = int(self.buffer[:separator])
                end = separator + 1 + length
                if len(self.buffer) >= end:
                    packet = json.loads(self.buffer[separator + 1 : end])
                    self.buffer = self.buffer[end:]
                    return packet
            chunk = self.socket.recv(65536)
            if not chunk:
                raise ConnectionClosed("The DevTools server closed the connection")
            self.buffer += chunk

    def receive_matching(self, predicate):
        while True:
            packet = self.receive()
            # The server replaces the frame target whenever a load finishes, and the new target's form can arrive
            # at any point of an exchange: follow it, as a client would, so that later requests go to the live target.
            if packet.get("type") == "target-available-form":
                self.console = packet["target"]["consoleActor"]
            if predicate(packet):
                return packet

    def tab(self):
        self.send({"to": "root", "type": "listTabs"})
        tabs = self.receive_matching(lambda packet: packet.get("from") == "root" and "tabs" in packet)["tabs"]
        assert len(tabs) == 1, tabs
        return tabs[0]

    def watch_frame_targets(self):
        tab = self.tab()["actor"]
        self.send({"to": tab, "type": "getWatcher"})
        watcher = self.receive_matching(lambda packet: packet.get("from") == tab and "actor" in packet)["actor"]
        self.send({"to": watcher, "type": "watchTargets", "targetType": "frame"})
        self.follow_frame_target()

    def follow_frame_target(self):
        self.receive_matching(lambda packet: packet.get("type") == "target-available-form")

    def evaluate(self, script):
        # A target switch between choosing the console and its answer loses the request: the old console is gone (an
        # unknownActor error naming it, with no resultID) or drops its pending result. Ask the new target's console
        # again then. An error for an earlier console is stale, and passes by.
        def unknown_actor(packet, console):
            return packet.get("error") == "unknownActor" and console in packet.get("message", "")

        while True:
            console = self.console
            self.send({"to": console, "type": "evaluateJSAsync", "text": script})
            packet = self.receive_matching(
                lambda packet, console=console: (
                    (packet.get("from") == console and "resultID" in packet)
                    or unknown_actor(packet, console)
                    or self.console != console
                )
            )
            if unknown_actor(packet, console) and self.console == console:
                self.follow_frame_target()
            if self.console != console:
                continue
            result_id = packet["resultID"]
            result = self.receive_matching(
                lambda packet, console=console, result_id=result_id: (
                    (packet.get("type") == "evaluationResult" and packet.get("resultID") == result_id)
                    or self.console != console
                )
            )
            if self.console != console:
                continue
            assert not result.get("hasException"), result.get("exceptionMessage")
            return result.get("result")

    def close(self):
        self.socket.close()


def wait_until(description, condition):
    deadline = time.monotonic() + TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        if condition():
            return
        time.sleep(0.05)
    raise AssertionError(f"Timed out waiting until {description}")


def wait_for_page(client, url, path):
    # The tab takes the title of the page's document once that document is active.
    def tab_shows_page():
        tab = client.tab()
        return tab.get("url") == url and tab.get("title") == path

    wait_until(f"the tab shows {url}", tab_shows_page)


def launch(ladybird_binary, profile, url):
    # Launches the browser with DevTools on a free port and connects the first client, whose connection doubles as the
    # check that the server is up: a throwaway probe would hold the server's one connection slot until it noticed the
    # probe had gone, and turn the client away meanwhile.
    # Another process can take the port between unused_port() and the server's bind(), so try another one then. The
    # attempts share one deadline, so a server that never comes up fails the test within it.
    deadline = time.monotonic() + TIMEOUT_SECONDS
    for _ in range(5):
        if time.monotonic() >= deadline:
            break
        devtools_port = helpers["unused_port"]()
        process = subprocess.Popen(
            [
                ladybird_binary,
                "--headless=manual",
                f"--profile-path={profile}",
                f"--devtools={devtools_port}",
                "--expose-internals-object",
                url,
            ],
            stderr=subprocess.PIPE,
            text=True,
        )
        bind_failed = threading.Event()

        def forward_stderr(stderr=process.stderr, bind_failed=bind_failed):
            assert stderr is not None
            for line in stderr:
                if line.startswith("Unable to launch devtools server"):
                    bind_failed.set()
                sys.stderr.write(line)

        threading.Thread(target=forward_stderr, daemon=True).start()

        while not bind_failed.is_set() and process.poll() is None and time.monotonic() < deadline:
            try:
                attempt_deadline = min(deadline, time.monotonic() + 1)
                return process, devtools_port, DevToolsClient(devtools_port, deadline=attempt_deadline)
            except OSError:
                pass
        stop(process)
    raise AssertionError("The DevTools server didn't start")


def stop(process):
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def assert_page_survives_disconnects(devtools_port, path, load):
    # The closed client disconnected DevTools from the page that displays the tab, so that page must still hold the
    # document it loaded: The tab would have reloaded into another page had it crashed.
    client = DevToolsClient(devtools_port)
    try:
        client.watch_frame_targets()
        assert client.evaluate("window.load") == load
    finally:
        client.close()
    assert load_count(path) == load, f"{path} was loaded {load_count(path)} times"


def test_cross_site_navigation(ladybird_binary, profile, server):
    source_url = f"http://127.0.0.1:{server.server_port}/source"
    target_url = f"http://localhost:{server.server_port}/target"
    process, devtools_port, client = launch(ladybird_binary, profile, source_url)
    try:
        try:
            wait_for_page(client, source_url, "/source")
            client.watch_frame_targets()

            # A cross-site navigation moves the tab into a page of another process.
            client.evaluate(f"setTimeout(() => location.href = {json.dumps(target_url)}, 0)")
            wait_for_page(client, target_url, "/target")
        finally:
            client.close()

        assert_page_survives_disconnects(devtools_port, "/target", 1)
        assert process.poll() is None, "The browser exited"
        print("PASS: A DevTools client follows the tab into a cross-site navigation's page")
    finally:
        stop(process)


def test_crash_recovery(ladybird_binary, profile, server):
    url = f"http://127.0.0.1:{server.server_port}/crash"
    process, devtools_port, client = launch(ladybird_binary, profile, url)
    try:
        try:
            wait_for_page(client, url, "/crash")
            client.watch_frame_targets()
            assert client.evaluate("window.load") == 1

            # The headless browser restores the crashed tab into a page of a new process, and the client inspects
            # that page's document once it's active.
            panic = "internals.panicRenderStateForTesting()"
            client.send({"to": client.console, "type": "evaluateJSAsync", "text": panic})
            client.follow_frame_target()
            assert client.evaluate("window.load") == 2
        finally:
            client.close()

        assert_page_survives_disconnects(devtools_port, "/crash", 2)
        assert process.poll() is None, "The browser exited"
        print("PASS: A DevTools client follows the tab into the page that replaces a crashed one")
    finally:
        stop(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-devtools-") as directory:
            test_cross_site_navigation(ladybird_binary, Path(directory) / "navigation", server)
            test_crash_recovery(ladybird_binary, Path(directory) / "crash", server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
