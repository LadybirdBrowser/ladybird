#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# When the Compositor dies, the browser connects a new one, and the page's next rendering update
# has to bring the new Compositor up to date: its first frame carries the whole display list again,
# not only what changed since the old Compositor's last frame. Every WebGL context of the page and
# of its worker is lost with the Compositor and restored in the new one, and a 2D canvas made after
# the reconnect draws in the new one.

import argparse
import base64
import importlib
import os
import shlex
import signal
import subprocess
import tempfile
import time
import urllib.parse

from pathlib import Path

webdriver_helpers = importlib.import_module("test-webdriver-delete-session")

PAGE = "data:text/html," + urllib.parse.quote(
    """<style>
#box { width: 100px; height: 100px; background: blue; }
</style><div id=box></div><canvas id=canvas style="display: none"></canvas><script>
window.webglContextEvents = [];
window.workerWebGLContext = "pending";
function createWebGLContextThatRecordsItsEvents(name, canvas) {
    canvas.addEventListener("webglcontextlost", event => {
        event.preventDefault();
        webglContextEvents.push(`${name} lost`);
    });
    canvas.addEventListener("webglcontextrestored", () => webglContextEvents.push(`${name} restored`));
    return canvas.getContext("webgl") !== null;
}
const workerSource = `
    const canvas = new OffscreenCanvas(16, 16);
    canvas.addEventListener("webglcontextlost", event => {
        event.preventDefault();
        postMessage("OffscreenCanvas in a worker lost");
    });
    canvas.addEventListener("webglcontextrestored", () => postMessage("OffscreenCanvas in a worker restored"));
    postMessage(canvas.getContext("webgl") !== null ? "created" : "not created");
`;
function pixelOfNew2DCanvas() {
    const context = document.createElement("canvas").getContext("2d");
    context.fillStyle = "rgb(0, 128, 0)";
    context.fillRect(0, 0, 1, 1);
    return Array.from(context.getImageData(0, 0, 1, 1).data).join(",");
}
function createWebGLContexts() {
    window.offscreenCanvas = new OffscreenCanvas(16, 16);
    window.worker = new Worker(URL.createObjectURL(new Blob([workerSource], { type: "application/javascript" })));
    worker.onmessage = event => {
        if (event.data === "created" || event.data === "not created")
            workerWebGLContext = event.data;
        else
            webglContextEvents.push(event.data);
    };
    return createWebGLContextThatRecordsItsEvents("canvas element", document.getElementById("canvas"))
        && createWebGLContextThatRecordsItsEvents("OffscreenCanvas", offscreenCanvas);
}
</script>"""
)


def descendants_named(root_pid, process_name):
    rows = subprocess.check_output(["ps", "-axo", "pid=,ppid=,command="], text=True).splitlines()
    children = {}
    for row in rows:
        fields = row.strip().split(None, 2)
        pid, parent = fields[:2]
        command = fields[2] if len(fields) == 3 else ""
        children.setdefault(int(parent), []).append((int(pid), command))
    pending = [root_pid]
    found = []
    while pending:
        for pid, command in children.get(pending.pop(), []):
            arguments = shlex.split(command)
            # A helper's own children, such as its file broker, carry the helper's command line but are not the helper.
            if arguments and Path(arguments[0]).name == process_name:
                found.append(pid)
            else:
                pending.append(pid)
    return found


def execute(port, session, script):
    status, payload, body = webdriver_helpers.request(
        port, "POST", f"/session/{session}/execute/sync", {"script": script, "args": []}
    )
    assert status == 200, body
    return payload["value"]


def wait_for_script_result(port, session, script, expected):
    deadline = time.monotonic() + webdriver_helpers.EVENT_TIMEOUT_SECONDS
    while True:
        result = execute(port, session, script)
        if result == expected:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"{script} gave {result}, not {expected}")
        time.sleep(0.05)


def take_screenshot(port, session):
    status, payload, body = webdriver_helpers.request(port, "GET", f"/session/{session}/screenshot")
    assert status == 200, body
    png = base64.b64decode(payload["value"])
    assert png.startswith(b"\x89PNG\r\n\x1a\n"), "Screenshot is not a PNG"
    return png


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory(prefix="ladybird-compositor-reconnect-") as temporary:
        environment = os.environ.copy()
        for variable, directory in (
            ("XDG_DATA_HOME", "data"),
            ("XDG_CONFIG_HOME", "config"),
            ("XDG_CACHE_HOME", "cache"),
        ):
            environment[variable] = str(Path(temporary) / directory)
        webdriver, port = webdriver_helpers.launch_webdriver(webdriver_binary, "--headless", env=environment)
        try:
            session = webdriver_helpers.create_session(port)
            status, _, body = webdriver_helpers.request(port, "POST", f"/session/{session}/url", {"url": PAGE})
            assert status == 200, body
            webdriver_helpers.wait_for_compositor(port, session)
            assert execute(port, session, "return createWebGLContexts();"), "WebGL contexts could not be created"
            wait_for_script_result(port, session, "return workerWebGLContext;", "created")
            assert execute(port, session, "return pixelOfNew2DCanvas();") == "0,128,0,255"
            first = take_screenshot(port, session)

            compositors = descendants_named(webdriver.pid, "Compositor")
            assert len(compositors) == 1, f"Expected one Compositor, found {len(compositors)}"
            os.kill(compositors[0], signal.SIGKILL)

            deadline = time.monotonic() + webdriver_helpers.EVENT_TIMEOUT_SECONDS
            while True:
                replacements = [pid for pid in descendants_named(webdriver.pid, "Compositor") if pid != compositors[0]]
                if replacements:
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError("No Compositor replaced the one that died")
                time.sleep(0.05)

            # The new Compositor paints the page as it was, then as it changes.
            assert take_screenshot(port, session) == first
            execute(port, session, "document.getElementById('box').style.background = 'green';")
            assert take_screenshot(port, session) != first
            assert execute(port, session, "return pixelOfNew2DCanvas();") == "0,128,0,255"

            expected_webgl_context_events = sorted(
                f"{context} {event}"
                for context in ("canvas element", "OffscreenCanvas", "OffscreenCanvas in a worker")
                for event in ("lost", "restored")
            )
            wait_for_script_result(
                port, session, "return webglContextEvents.toSorted();", expected_webgl_context_events
            )
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
    args = parser.parse_args()
    run_test(args.webdriver_binary)
