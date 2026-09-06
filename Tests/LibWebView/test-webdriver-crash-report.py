#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import os
import re
import shlex
import signal
import subprocess
import tempfile
import time

from pathlib import Path

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")


def crash_helper(webdriver_pid, process_name):
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines()
    children = {}
    for row in rows:
        fields = row.strip().split(None, 2)
        pid, parent = fields[:2]
        command = fields[2] if len(fields) == 3 else ""
        children.setdefault(int(parent), []).append((int(pid), command))
    pending = [webdriver_pid]
    targets = []
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            arguments = shlex.split(command)
            if arguments and Path(arguments[0]).name == process_name:
                assert "--crash-report-fd" in arguments, f"{process_name} has no capture descriptor"
                assert "--disable-sandbox" not in arguments, f"{process_name} is not sandboxed"
                targets.append(pid)
    assert len(targets) == 1, f"Expected one {process_name} child, found {len(targets)}"
    os.kill(targets[0], signal.SIGSEGV)


def run_test(webdriver_binary, process_name):
    with tempfile.TemporaryDirectory(prefix="ladybird-crash-report-") as temporary:
        environment = os.environ.copy()
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        port = webdriver_helpers.unused_port()
        webdriver = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port)], env=environment
        )
        try:
            webdriver_helpers.wait_for_port(port)
            status, payload, body = webdriver_helpers.request(
                port,
                "POST",
                "/session",
                {"capabilities": {"alwaysMatch": {"ladybird:headless": True, "ladybird:enableTestHooks": True}}},
            )
            assert status == 200, body
            session = payload["value"]["sessionId"]
            status, _, body = webdriver_helpers.request(
                port,
                "POST",
                f"/session/{session}/url",
                {"url": "data:text/html,<title>PRIVATE_CRASH_TITLE</title>PRIVATE_CRASH_CONTENT"},
            )
            assert status == 200, body
            if process_name == "WebContent":
                status, _, body = webdriver_helpers.request(
                    port, "POST", f"/session/{session}/ladybird/crash-current-page", {}
                )
                assert status == 200, body
            else:
                if process_name == "WebWorker":
                    status, payload, body = webdriver_helpers.request(
                        port,
                        "POST",
                        f"/session/{session}/execute/async",
                        {
                            "script": """
                            const done = arguments[0];
                            window.worker = new Worker(URL.createObjectURL(new Blob([
                                "postMessage('ready'); onmessage = () => {};"
                            ], {type: 'text/javascript'})));
                            worker.onmessage = () => done();
                            worker.onerror = () => done('worker failed');
                        """,
                            "args": [],
                        },
                    )
                    assert status == 200 and payload == {"value": None}, body
                crash_helper(webdriver.pid, process_name)
            directory = Path(temporary) / "data/Ladybird/CrashReports"
            deadline = time.monotonic() + webdriver_helpers.EVENT_TIMEOUT_SECONDS
            while True:
                reports = list(directory.glob(f"*-{process_name}-*.txt"))
                if reports:
                    text = reports[0].read_text()
                    if "Stacks may be partial." in text:
                        break
                if time.monotonic() >= deadline:
                    raise AssertionError("No complete crash report was saved")
                time.sleep(0.05)
            assert re.fullmatch(
                rf"\d{{4}}-\d{{2}}-\d{{2}}T\d{{2}}-\d{{2}}-\d{{2}}Z-{process_name}-[A-Za-z0-9]{{6}}\.txt",
                reports[0].name,
            ), reports[0].name
            assert f"Process: {process_name}\n" in text, text
            assert "Captured signal:" in text, text
            captured_signal = re.search(r"^Captured signal: (.+)$", text, re.MULTILINE)
            assert captured_signal is not None, text
            assert f"Termination signal: {captured_signal.group(1)}\n" in text, text
            if process_name == "WebContent":
                assert "Verification failed: false at Services/WebContent/WebDriverConnection.cpp:" in text, text
            else:
                assert "Verification failed:" not in text and "Assertion failed:" not in text, text
            assert "Executable build ID:" in text, text
            assert re.search(r"^Git commit: ([0-9a-f]{40}|[0-9a-f]{64}|unknown)$", text, re.MULTILINE), text
            assert "C++ compiler:" in text and "C++ flags (" in text and "Build options:" in text, text
            assert "#0 " in text and "#1 " in text, text
            assert " + 0x" in text or " at Services/" in text, text
            assert "PRIVATE_CRASH" not in text, text
            assert temporary not in text and str(Path.home()) not in text, text
            assert reports[0].stat().st_mode & 0o777 == 0o600
            assert directory.stat().st_mode & 0o777 == 0o700
            webdriver_helpers.request(port, "DELETE", f"/session/{session}")
        finally:
            webdriver.terminate()
            try:
                webdriver.wait(timeout=5)
            except subprocess.TimeoutExpired:
                webdriver.kill()
                webdriver.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    parser.add_argument(
        "--process",
        default="WebContent",
        choices=["WebContent", "WebWorker", "RequestServer", "ImageDecoder", "Compositor", "WasmCompiler"],
    )
    args = parser.parse_args()
    run_test(args.webdriver_binary, args.process)
