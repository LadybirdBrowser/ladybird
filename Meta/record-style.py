#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Record headless website loads for standalone StyleEngine replay."""

import argparse
import json
import os
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from pathlib import Path

DEFAULT_WEBDRIVER = "Build/release/bin/Ladybird.app/Contents/MacOS/WebDriver"


def request_json(base_url, method, path, body=None, timeout=120):
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        base_url + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.load(response)


def unused_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def wait_for_webdriver(base_url, process):
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError("WebDriver exited before accepting connections")
        try:
            request_json(base_url, "GET", "/status", timeout=1)
            return
        except (OSError, urllib.error.URLError):
            time.sleep(0.05)
    raise RuntimeError("WebDriver did not accept connections within 10 seconds")


def stop_process(process):
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def record(arguments):
    output = Path(arguments.output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        raise RuntimeError(f"capture directory is not empty: {output}")

    environment = os.environ.copy()
    environment["LIBWEB_STYLE_RECORD"] = str(output / "stylegraph-%p.sg")
    port = unused_port()
    base_url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="ladybird-stylegraph-record-") as profiles_directory:
        process = subprocess.Popen(
            [
                arguments.webdriver,
                "--headless",
                "--disable-sandbox",
                "--profiles-directory",
                profiles_directory,
                "--port",
                str(port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=None if arguments.keep_stderr else subprocess.DEVNULL,
            env=environment,
        )
        session = None
        try:
            wait_for_webdriver(base_url, process)
            session = request_json(
                base_url,
                "POST",
                "/session",
                {"capabilities": {"alwaysMatch": {}}},
            )["value"]["sessionId"]
            request_json(
                base_url,
                "POST",
                f"/session/{session}/timeouts",
                {"script": arguments.timeout * 1000, "pageLoad": arguments.timeout * 1000},
            )
            for url in arguments.urls:
                request_json(base_url, "POST", f"/session/{session}/url", {"url": url})
                if arguments.settle_seconds:
                    time.sleep(arguments.settle_seconds)
                title = request_json(base_url, "GET", f"/session/{session}/title")["value"]
                print(f"loaded {url}: {title}")
        finally:
            if session is not None:
                try:
                    request_json(base_url, "DELETE", f"/session/{session}", timeout=10)
                except (OSError, urllib.error.URLError):
                    pass
            stop_process(process)

    captures = sorted(output.glob("stylegraph-*.sg"))
    if not captures:
        raise RuntimeError("the browser produced no capture")
    for capture in captures:
        print(f"recorded {capture} ({capture.stat().st_size} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="new or empty directory for capture streams")
    parser.add_argument("urls", nargs="+", help="URLs to load in order")
    parser.add_argument("--webdriver", default=DEFAULT_WEBDRIVER, help="WebDriver executable")
    parser.add_argument("--timeout", type=int, default=120, help="navigation timeout in seconds")
    parser.add_argument("--settle-seconds", type=float, default=2, help="time to record after each load")
    parser.add_argument("--keep-stderr", action="store_true", help="show browser stderr")
    record(parser.parse_args())


if __name__ == "__main__":
    main()
