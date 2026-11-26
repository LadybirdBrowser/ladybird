#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# What a page logs to its console, and the exceptions it doesn't catch, also go to the terminal Ladybird runs in. The
# page decides what's in them, so a control character in one must come out as an escape: Written as it is, it would
# take effect in the terminal.

import argparse
import http.server
import queue
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

TIMEOUT_SECONDS = 60
DONE = "console-escapes-done"

PAGE = b"""<!doctype html><title>console-escapes</title>
<script>
console.log("page-log[\\x1b[2J]");
console.warn("page-warn[\\x1b]0;title\\x07]");
console.error("page-error[\\x9b2J]");
console.group("page-group[\\x1b[31m]");
console.groupEnd();
console.trace("page-trace[\\x1b[H]");
new Worker("/worker.js").onmessage = () => {
    setTimeout(() => { throw new Error("page-exception[\\x1b[2J]"); });
    setTimeout(() => { throw "page-thrown-string[\\x1b[2J]"; });
    setTimeout(() => console.log("%s", "%DONE%"), 100);
};
</script>""".replace(b"%DONE%", DONE.encode())
WORKER = b"""
console.clear();
console.log("worker-log[\\x1b[2J]");
console.group("worker-group[\\r\\x1b[31m]");
console.groupEnd();
console.trace("worker-trace[\\x1b[H]");
postMessage("logged");
"""

# The colors Ladybird gives its own prefixes and labels.
OWN_COLORS = ["\x1b[0m", "\x1b[31;1m", "\x1b[32;1m", "\x1b[36;1m"]

# Each page-chosen string, as its control characters must appear in the output.
EXPECTED = [
    "page-log[\\x1b[2J]",
    "page-warn[\\x1b]0;title\\x07]",
    "page-error[\\x9b2J]",
    "page-group[\\x1b[31m]",
    "page-trace[\\x1b[H]",
    "page-exception[\\x1b[2J]",
    "page-thrown-string[\\x1b[2J]",
    "worker-log[\\x1b[2J]",
    "worker-group[\\x0d\\x1b[31m]",
    "worker-trace[\\x1b[H]",
]


def is_control_character(character):
    code_point = ord(character)
    return (code_point < 0x20 and character != "\t") or 0x7F <= code_point < 0xA0


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript" if self.path == "/worker.js" else "text/html")
        self.end_headers()
        self.wfile.write(WORKER if self.path == "/worker.js" else PAGE)

    def log_message(self, format, *args):
        pass


def read_output_until_done(process):
    lines = queue.Queue()

    def forward():
        # Read bytes and split them only at newlines: Text mode would turn a carriage return into a newline, and the
        # check below couldn't see it.
        for line in process.stderr:
            lines.put(line.decode("utf-8", errors="surrogateescape"))
        lines.put(None)

    threading.Thread(target=forward, daemon=True).start()
    output = []
    deadline = time.monotonic() + TIMEOUT_SECONDS
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=deadline - time.monotonic())
        except queue.Empty:
            break
        if line is None:
            break
        output.append(line)
        if DONE in line:
            return "".join(output)
    sys.stderr.write("".join(output))
    raise AssertionError("the page never logged that it was done")


def find_failures(output):
    failures = [expected for expected in EXPECTED if expected not in output]
    # The escapes Ladybird adds itself color the prefixes, so look for raw control characters only where the page's
    # strings are.
    for line in output.split("\n"):
        starts = [line.find(tag) for tag in ("page-", "worker-") if tag in line]
        if not starts:
            continue
        payload = line[min(starts) :]
        for own_color in OWN_COLORS:
            payload = payload.replace(own_color, "")
        if any(is_control_character(character) for character in payload):
            failures.append(f"raw control character in: {line!r}")
    if "\x1b[3J" in output or "\x1b[H\x1b[2J" in output:
        failures.append("a worker's console.clear() wrote a clear-screen sequence")
    return failures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary
    server = helpers["LocalHTTPServer"](("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_port}/page"
    try:
        with tempfile.TemporaryDirectory(prefix="ladybird-console-escapes-") as profile:
            process = subprocess.Popen(
                [ladybird_binary, "--headless=manual", f"--profile-path={profile}", url],
                stderr=subprocess.PIPE,
            )
            try:
                output = read_output_until_done(process)
            finally:
                process.kill()
                process.wait()
    finally:
        server.shutdown()

    failures = find_failures(output)
    if failures:
        sys.stderr.write(output)
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
