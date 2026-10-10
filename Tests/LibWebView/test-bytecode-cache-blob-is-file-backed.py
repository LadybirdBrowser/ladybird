#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import glob
import http.server
import os
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

FUNCTION_COUNT = 4000

# A script whose bytecode cache blob runs to several MB: Every function has a body of its own.
SCRIPT = (
    "\n".join(
        f"function f{i}(a, b) {{ let s = {i}; for (let j = 0; j < a; ++j) s += (j * {i + 1}) % (b + {i % 7 + 1}); return s + 'f{i}'.length; }}"
        for i in range(FUNCTION_COUNT)
    )
    + "\nwindow.scriptRan = f1(2, 3) + f2(3, 4);\n"
).encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/script.js":
            body = SCRIPT
            content_type = "text/javascript"
        elif self.path == "/":
            body = b'<!doctype html><title>blob</title><script src="/script.js"></script>'
            content_type = "text/html"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "max-age=3600")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def web_content_pids(root_pid):
    output = subprocess.run(["ps", "-axo", "pid=,ppid=,command="], capture_output=True, text=True).stdout
    children = {}
    for pid, ppid, command in (line.split(None, 2) for line in output.splitlines()):
        children.setdefault(int(ppid), []).append((int(pid), command))
    found = []
    pending = [root_pid]
    while pending:
        for pid, command in children.get(pending.pop(), []):
            pending.append(pid)
            if command.split()[0].endswith("/WebContent"):
                found.append(pid)
    return found


def mapped_files(pid):
    # The paths of the files the process has mapped.
    if sys.platform == "darwin":
        output = subprocess.run(["vmmap", "-wide", str(pid)], capture_output=True, text=True).stdout
        paths = [line.rsplit(None, 1)[-1] for line in output.splitlines() if line.startswith("mapped file")]
        return [path for path in paths if path.startswith("/")]
    paths = []
    for line in open(f"/proc/{pid}/maps"):
        fields = line.split(None, 5)
        if len(fields) == 6 and fields[5].startswith("/"):
            paths.append(fields[5].strip())
    return paths


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    webdriver_binary = parser.parse_args().webdriver_binary

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-bytecode-cache-") as profile:
            port = helpers["unused_port"]()
            process = subprocess.Popen(
                [webdriver_binary, "--headless", f"--profile-path={profile}", "-l", "127.0.0.1", "-p", str(port)]
            )
            session = None
            try:
                helpers["wait_for_port"](port)
                session = helpers["create_session"](port)
                status, _, raw = helpers["request"](
                    port, "POST", f"/session/{session}/url", {"url": f"http://127.0.0.1:{server.server_port}/"}
                )
                assert status == 200, raw

                # The first load of a fresh profile generates the script's bytecode cache blob and stores it in the
                # profile's HTTP cache, after the script has run.
                deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
                payload = {"value": None}
                blobs = []
                while time.monotonic() < deadline:
                    status, payload, raw = helpers["request"](
                        port,
                        "POST",
                        f"/session/{session}/execute/sync",
                        {"script": "return window.scriptRan", "args": []},
                    )
                    assert status == 200, raw
                    blobs = [
                        path
                        for path in glob.glob(os.path.join(profile, "**", "*.jsbc"), recursive=True)
                        if os.path.getsize(path) > 0
                    ]
                    if payload["value"] is not None and blobs:
                        break
                    time.sleep(0.2)
                assert payload["value"] is not None, "The script never ran"
                assert len(blobs) == 1, f"Expected one stored bytecode cache blob, found {blobs}"
                blob = os.path.realpath(blobs[0])
                blob_size = os.path.getsize(blob)

                # The script's executables are backed by a mapping of the stored blob, not by a heap copy of it.
                pids = web_content_pids(process.pid)
                assert pids, "No WebContent process"
                deadline = time.monotonic() + helpers["EVENT_TIMEOUT_SECONDS"]
                mapped = []
                while time.monotonic() < deadline:
                    mapped = [path for pid in pids for path in mapped_files(pid) if os.path.realpath(path) == blob]
                    if mapped:
                        break
                    time.sleep(0.2)
                assert mapped, f"WebContent {pids} never mapped the stored blob {blob} ({blob_size} bytes)"
                print(f"PASS: The generated bytecode cache blob ({blob_size >> 10} KiB) is mapped from the cache file")
            finally:
                try:
                    if session is not None:
                        helpers["request"](port, "DELETE", f"/session/{session}")
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
