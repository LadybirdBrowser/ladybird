#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import functools
import http.server
import json
import runpy
import subprocess
import tempfile
import threading

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


def run_browser(webdriver_binary, settings, check):
    with tempfile.TemporaryDirectory() as directory:
        site = Path(directory) / "site"
        site.mkdir()
        (site / "index.html").write_text("<!doctype html><title>site</title>")
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(QuietHandler, directory=str(site)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(json.dumps(settings))
        process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(path, body=None, method="POST"):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def switch_state():
                command("url", {"url": "about:settings"})
                return command(
                    "execute/async",
                    {
                        "script": """
                        const done = arguments[0];
                        const toggle = document.querySelector('#enable-microphone');
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (event.detail.name !== 'loadSettings') return;
                            document.removeEventListener('WebUIMessage', listener);
                            setTimeout(() => done(toggle.checked));
                        });
                        // The page may have loaded its settings already, so ask for them again.
                        if (window.ladybird) ladybird.sendMessage('loadCurrentSettings');
                    """,
                        "args": [],
                    },
                )

            def toggle_switch():
                command(
                    "execute/async",
                    {
                        "script": """
                        const done = arguments[0];
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (event.detail.name !== 'loadSettings') return;
                            document.removeEventListener('WebUIMessage', listener);
                            done();
                        });
                        document.querySelector('#enable-microphone').click();
                    """,
                        "args": [],
                    },
                )

            def site_permission():
                command("url", {"url": f"http://127.0.0.1:{server.server_address[1]}/index.html"})
                return command(
                    "execute/async",
                    {
                        "script": """
                        const done = arguments[0];
                        navigator.permissions.query({ name: 'microphone' }).then(status => done(status.state));
                    """,
                        "args": [],
                    },
                )

            check(switch_state, toggle_switch, site_permission, settings_file)
        finally:
            server.shutdown()
            server.server_close()
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def run_test(webdriver_binary):
    def check_fresh_profile(switch_state, toggle_switch, site_permission, settings_file):
        assert switch_state(), "a profile that never chose a microphone policy should let sites ask"
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("microphonePolicy") == "block", saved
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("microphonePolicy") == "ask", saved
        assert site_permission() == "prompt", "a site should be able to ask while the policy is ask"

    run_browser(webdriver_binary, {"firstRunComplete": True}, check_fresh_profile)

    def check_blocking_profile(switch_state, toggle_switch, site_permission, settings_file):
        assert not switch_state(), "a profile whose microphone policy is block should keep sites from asking"
        assert site_permission() == "denied", "a site should read as denied while the policy is block"

    run_browser(webdriver_binary, {"firstRunComplete": True, "microphonePolicy": "block"}, check_blocking_profile)
    print("PASS: the microphone policy defaults to letting sites ask, and a saved block stays")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
