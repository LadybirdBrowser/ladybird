#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

"""Measure repeated stylesheet parsing through CSSStyleSheet.replaceSync()."""

import argparse
import json
import os
import socket
import statistics
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

from pathlib import Path

DEFAULT_WEBDRIVER = "Build/release/bin/WebDriver"
DEFAULT_STYLESHEET = "Tests/LibWeb/WPT/wpt/tools/runner/css/bootstrap.min.css"

BENCHMARK_SCRIPT = r"""
const source = window.benchmarkSource;
const iterations = arguments[0];
const parsesPerIteration = arguments[1];
const warmupIterations = arguments[2];

function parseStylesheets(count) {
    let ruleCount = 0;
    for (let index = 0; index < count; ++index) {
        const sheet = new CSSStyleSheet();
        sheet.replaceSync(source);
        ruleCount += sheet.cssRules.length;
    }
    return ruleCount;
}

parseStylesheets(warmupIterations * parsesPerIteration);

const samples = [];
let ruleCount = 0;
for (let iteration = 0; iteration < iterations; ++iteration) {
    const before = performance.now();
    ruleCount += parseStylesheets(parsesPerIteration);
    samples.push(performance.now() - before);
}
return JSON.stringify({ ruleCount, samples });
"""


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


def run_benchmark(webdriver, source, iterations, parses_per_iteration, warmup_iterations, keep_stderr):
    port = unused_port()
    base_url = f"http://127.0.0.1:{port}"
    with tempfile.TemporaryDirectory(prefix="ladybird-css-value-parsing-") as temporary_directory:
        temporary_path = Path(temporary_directory)
        profiles_directory = temporary_path / "profiles"
        benchmark_page = temporary_path / "benchmark.html"
        escaped_source = json.dumps(source).replace("</", "<\\/")
        benchmark_page.write_text(f"<!doctype html><script>window.benchmarkSource = {escaped_source};</script>")
        process = subprocess.Popen(
            [
                webdriver,
                "--headless",
                "--disable-sandbox",
                "--profiles-directory",
                profiles_directory,
                "--port",
                str(port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=None if keep_stderr else subprocess.DEVNULL,
            env=os.environ.copy(),
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
                {"script": 600_000},
            )
            request_json(
                base_url,
                "POST",
                f"/session/{session}/url",
                {"url": benchmark_page.as_uri()},
            )
            value = request_json(
                base_url,
                "POST",
                f"/session/{session}/execute/sync",
                {
                    "script": BENCHMARK_SCRIPT,
                    "args": [iterations, parses_per_iteration, warmup_iterations],
                },
            )["value"]
            return json.loads(value)
        finally:
            if session is not None:
                try:
                    request_json(base_url, "DELETE", f"/session/{session}", timeout=10)
                except (OSError, urllib.error.URLError):
                    pass
            stop_process(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--webdriver", default=DEFAULT_WEBDRIVER, help="WebDriver executable")
    parser.add_argument("--stylesheet", default=DEFAULT_STYLESHEET, help="CSS corpus to parse")
    parser.add_argument("--iterations", type=int, default=25, help="number of measured samples")
    parser.add_argument("--parses-per-iteration", type=int, default=5, help="stylesheet parses per sample")
    parser.add_argument("--warmup-iterations", type=int, default=5, help="unmeasured samples")
    parser.add_argument("--keep-stderr", action="store_true", help="show browser stderr")
    args = parser.parse_args()

    if args.iterations < 1 or args.parses_per_iteration < 1 or args.warmup_iterations < 0:
        parser.error("iterations and parses per iteration must be positive, and warmup iterations non-negative")

    webdriver = Path(args.webdriver).resolve()
    stylesheet = Path(args.stylesheet).resolve()
    if not webdriver.is_file():
        parser.error(f"WebDriver executable not found: {webdriver}")
    if not stylesheet.is_file():
        parser.error(f"stylesheet not found: {stylesheet}")

    source = stylesheet.read_text()
    result = run_benchmark(
        str(webdriver),
        source,
        args.iterations,
        args.parses_per_iteration,
        args.warmup_iterations,
        args.keep_stderr,
    )
    samples = result["samples"]
    bytes_per_iteration = len(source.encode()) * args.parses_per_iteration
    print(
        json.dumps(
            {
                "stylesheet": str(stylesheet),
                "stylesheet_bytes": len(source.encode()),
                "iterations": args.iterations,
                "parses_per_iteration": args.parses_per_iteration,
                "warmup_iterations": args.warmup_iterations,
                "parsed_rule_count": result["ruleCount"],
                "milliseconds": {
                    "minimum": min(samples),
                    "median": statistics.median(samples),
                    "maximum": max(samples),
                },
                "median_megabytes_per_second": bytes_per_iteration / statistics.median(samples) / 1000,
                "samples": samples,
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
