#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import runpy
import subprocess
import tempfile

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_browser(webdriver_binary, settings, check):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(json.dumps(settings))
        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
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
                        const toggle = document.querySelector('#enable-notifications');
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
                        document.querySelector('#enable-notifications').click();
                    """,
                        "args": [],
                    },
                )

            check(switch_state, toggle_switch, settings_file)
        finally:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


def run_test(webdriver_binary):
    def check_fresh_profile(switch_state, toggle_switch, settings_file):
        assert switch_state(), "a profile that never chose a notifications policy should let sites ask"
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("notificationsPolicy") == "block", saved
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("notificationsPolicy") == "ask", saved

    run_browser(webdriver_binary, {"firstRunComplete": True}, check_fresh_profile)

    def check_blocking_profile(switch_state, toggle_switch, settings_file):
        assert not switch_state(), "a profile whose notifications policy is block should keep sites from asking"

    run_browser(webdriver_binary, {"firstRunComplete": True, "notificationsPolicy": "block"}, check_blocking_profile)
    print("PASS: the notifications policy defaults to letting sites ask, and a saved block stays")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
