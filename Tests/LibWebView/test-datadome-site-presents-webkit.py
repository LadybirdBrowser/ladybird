#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import http.server
import json
import runpy
import subprocess
import sys
import tempfile
import threading
import time

from pathlib import Path

devtools = runpy.run_path(str(Path(__file__).with_name("test-devtools-client-follows-the-tab.py")))
DevToolsClient = devtools["DevToolsClient"]
helpers = devtools["helpers"]
stop = devtools["stop"]
wait_until = devtools["wait_until"]
TIMEOUT_SECONDS = helpers["EVENT_TIMEOUT_SECONDS"]

# The User-Agent of every request each path received, in order, and the Accept header of each.
requests_seen: dict[str, list[str]] = {}
accepts_seen: dict[str, list[str]] = {}
requests_seen_lock = threading.Lock()


def is_webkit(user_agent):
    return (
        "AppleWebKit/605.1.15" in user_agent
        and "Version/" in user_agent
        and "Safari/605.1.15" in user_agent
        and "Ladybird/" not in user_agent
        and "Chrome/" not in user_agent
    )


FRAME = b"""<!doctype html><title>frame</title><script>
parent.postMessage(JSON.stringify({ kind: "frame", userAgent: navigator.userAgent, hasDeviceMemory: "deviceMemory" in navigator }), "*");
</script>"""

# The page embeds a cross-site frame of the same server (localhost against 127.0.0.1, so another process) and starts a
# worker; both report their navigator, and the page gathers everything into window.report.
PAGE = b"""<!doctype html><title>/page</title><body><script>
const report = { page: { userAgent: navigator.userAgent, hasDeviceMemory: "deviceMemory" in navigator } };
addEventListener("message", event => {
    const data = JSON.parse(event.data);
    report[data.kind] = data;
    if (report.frame && report.worker) window.report = JSON.stringify(report);
});
const worker = new Worker(URL.createObjectURL(new Blob(["postMessage(navigator.userAgent)"])));
worker.onmessage = event => {
    report.worker = { kind: "worker", userAgent: event.data };
    if (report.frame && report.worker) window.report = JSON.stringify(report);
};
const frame = document.createElement("iframe");
frame.src = "http://localhost:" + location.port + "/frame";
document.body.appendChild(frame);
</script>"""


class DataDomeHandler(http.server.BaseHTTPRequestHandler):
    # A site behind DataDome: every response carries its marker, served pages and challenges alike.
    def do_GET(self):
        with requests_seen_lock:
            requests_seen.setdefault(self.path, []).append(self.headers.get("User-Agent", ""))
            accepts_seen.setdefault(self.path, []).append(self.headers.get("Accept", ""))
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.path == "/entry":
            self.send_response(302)
            self.send_header("Location", "/page")
            self.send_header("Cache-Control", "no-store")
            self.send_header("x-datadome", "protected")
            self.end_headers()
            return
        body = (
            PAGE
            if self.path == "/page"
            else FRAME
            if self.path == "/frame"
            else f"<!doctype html><title>{self.path}</title>".encode()
        )
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.send_header("x-datadome", "protected")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


class PlainHandler(http.server.BaseHTTPRequestHandler):
    # A site with no bot detection at all, which can also redirect to the DataDome one.
    redirect_to = ""

    def do_GET(self):
        with requests_seen_lock:
            requests_seen.setdefault("plain" + self.path, []).append(self.headers.get("User-Agent", ""))
        if self.path == "/favicon.ico":
            self.send_error(404)
            return
        if self.path == "/redirect":
            self.send_response(302)
            self.send_header("Location", PlainHandler.redirect_to)
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b"<!doctype html><title>/plain</title>")

    def log_message(self, format, *args):
        pass


def launch(ladybird_binary, profile, url):
    # Like the DevTools tests' launch(), with no spoofing set: the identity has to come from the site alone.
    deadline = time.monotonic() + TIMEOUT_SECONDS
    for _ in range(5):
        if time.monotonic() >= deadline:
            break
        devtools_port = helpers["unused_port"]()
        process = subprocess.Popen(
            [ladybird_binary, "--headless=manual", f"--profile-path={profile}", f"--devtools={devtools_port}", url],
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
                return process, DevToolsClient(devtools_port, deadline=attempt_deadline)
            except OSError:
                pass
        stop(process)
    raise AssertionError("The DevTools server didn't start")


def user_agents_for(path):
    with requests_seen_lock:
        return list(requests_seen.get(path, []))


def test_a_datadome_site_is_presented_webkit(ladybird_binary, profile, datadome, plain):
    base = f"http://127.0.0.1:{datadome.server_port}"
    process, client = launch(ladybird_binary, profile, f"{base}/entry")
    try:
        wait_until("the tab shows the page", lambda: client.tab().get("title") == "/page")
        client.watch_frame_targets()

        # The first request went out under our own name; the marker on its response had it sent again as WebKit's
        # browser, and the page, its cross-site frame, and its worker all say so. The retry's Accept header is the
        # WebKit mode's too, not the one the first request carried. And the redirect the retry got stays within the
        # site the navigation just learned, so the page's one request is WebKit's too, whether or not the learned
        # rule has reached the process by then.
        [own, webkit] = user_agents_for("/entry")
        assert "Ladybird/" in own, own
        assert is_webkit(webkit), webkit
        with requests_seen_lock:
            [first_accept, retry_accept] = accepts_seen["/entry"]
        assert "image/apng" in first_accept, first_accept
        assert retry_accept == "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", retry_accept
        page = user_agents_for("/page")
        assert len(page) == 1 and is_webkit(page[0]), page
        wait_until("the frame and the worker report", lambda: isinstance(client.evaluate("window.report"), str))
        report = json.loads(client.evaluate("window.report"))
        assert is_webkit(report["page"]["userAgent"]) and not report["page"]["hasDeviceMemory"], report["page"]
        assert is_webkit(report["frame"]["userAgent"]) and not report["frame"]["hasDeviceMemory"], report["frame"]
        assert is_webkit(report["worker"]["userAgent"]), report["worker"]
        assert all(is_webkit(user_agent) for user_agent in user_agents_for("/frame")), user_agents_for("/frame")

        # A site with no bot detection (another host: a learned site is a host, whatever its port) gets our own name
        # again, in a page with the default mode.
        client.evaluate(f"location.assign('http://localhost:{plain.server_port}/plain')")
        wait_until("the plain page shows", lambda: client.tab().get("title") == "/plain")
        assert "Ladybird/" in client.evaluate("navigator.userAgent")
        assert client.evaluate("'deviceMemory' in navigator") is True
        assert all("Ladybird/" in user_agent for user_agent in user_agents_for("plain/plain")), user_agents_for(
            "plain/plain"
        )
    finally:
        client.close()
        stop(process)

    # The profile remembers the site: a fresh launch presents WebKit's browser from its first request.
    process, client = launch(ladybird_binary, profile, f"{base}/second")
    try:
        wait_until("the second page shows", lambda: client.tab().get("title") == "/second")
        client.watch_frame_targets()
        second = user_agents_for("/second")
        assert len(second) == 1 and is_webkit(second[0]), second

        # And a redirect into the remembered site presents WebKit's browser to it from the redirected request on.
        PlainHandler.redirect_to = f"{base}/redirected"
        client.evaluate(f"location.assign('http://localhost:{plain.server_port}/redirect')")
        wait_until("the redirected page shows", lambda: client.tab().get("title") == "/redirected")
        redirected = user_agents_for("/redirected")
        assert len(redirected) == 1 and is_webkit(redirected[0]), redirected
        with requests_seen_lock:
            [redirected_accept] = accepts_seen["/redirected"]
        assert redirected_accept == "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8", redirected_accept

        print(
            "PASS: A DataDome-protected site gets WebKit's browser on this platform, in every process of the tab, and is remembered"
        )
    finally:
        client.close()
        stop(process)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    datadome = http.server.ThreadingHTTPServer(("127.0.0.1", 0), DataDomeHandler)
    plain = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PlainHandler)
    for server in (datadome, plain):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory() as directory:
            test_a_datadome_site_is_presented_webkit(ladybird_binary, Path(directory) / "profile", datadome, plain)
    finally:
        for server in (datadome, plain):
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
