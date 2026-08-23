#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Record a canonical headless StyleBench run for StyleEngine replay."""

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
DEFAULT_URL = "http://localhost:8000/?iterationCount=1"


def request_json(base_url, method, path, body=None, timeout=600):
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
    environment["LIBWEB_STYLE_RECORD"] = str(output / "stylebench-%p.sg")
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
                {"script": 600_000, "pageLoad": 600_000},
            )
            request_json(base_url, "POST", f"/session/{session}/url", {"url": arguments.url})
            suite = json.dumps(arguments.suite)
            start_script = f"""
const wanted = {suite};
if (wanted !== null) {{
    if (!Suites.some(suite => suite.name === wanted))
        throw new Error(`Unknown StyleBench suite: ${{wanted}}`);
    for (const suite of Suites)
        suite.disabled = suite.name !== wanted;
}}
window.__styleBenchFinished = false;
const didFinishLastIteration = benchmarkClient.didFinishLastIteration;
benchmarkClient.didFinishLastIteration = function (...args) {{
    window.__styleBenchFinished = true;
    return didFinishLastIteration.apply(this, args);
}};
return {{ started: startBenchmark(), count: benchmarkClient.stepCount }};
"""
            started = request_json(
                base_url,
                "POST",
                f"/session/{session}/execute/sync",
                {"script": start_script, "args": []},
            )["value"]
            if not started["started"]:
                raise RuntimeError("StyleBench refused to start")
            state = {"finished": 0, "total": started["count"]}
            deadline = time.monotonic() + arguments.timeout
            while time.monotonic() < deadline:
                state = request_json(
                    base_url,
                    "POST",
                    f"/session/{session}/execute/sync",
                    {
                        "script": "return { done: window.__styleBenchFinished === true, finished: benchmarkClient._finishedTestCount, total: benchmarkClient.stepCount };",
                        "args": [],
                    },
                )["value"]
                # NB: _finishedTestCount overshoots stepCount (it counts more than one event per step),
                #     so completion is signalled by didFinishLastIteration rather than by comparing counters.
                if state["done"]:
                    break
                time.sleep(0.25)
            else:
                raise RuntimeError(f"StyleBench timed out after {state['finished']} of {state['total']} tests")
        finally:
            if session is not None:
                try:
                    request_json(base_url, "DELETE", f"/session/{session}", timeout=10)
                except (OSError, urllib.error.URLError):
                    pass
            stop_process(process)

    captures = sorted(output.glob("stylebench-*.sg"))
    if not captures:
        raise RuntimeError("the browser produced no capture")
    print(f"recorded {state['total']} StyleBench subtests:")
    for capture in captures:
        print(f"  {capture} ({capture.stat().st_size} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", help="new or empty directory for capture streams")
    parser.add_argument("--suite", help="record only this exact StyleBench suite name")
    parser.add_argument("--webdriver", default=DEFAULT_WEBDRIVER, help="WebDriver executable")
    parser.add_argument("--url", default=DEFAULT_URL, help="canonical StyleBench runner URL")
    parser.add_argument(
        "--timeout",
        type=float,
        default=300,
        help="benchmark timeout in seconds (a full StyleBench run takes well under a minute)",
    )
    parser.add_argument("--keep-stderr", action="store_true", help="show browser stderr")
    record(parser.parse_args())


if __name__ == "__main__":
    main()
