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
                        const toggle = document.querySelector('#enable-geolocation');
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (event.detail.name !== 'loadSettings') return;
                            document.removeEventListener('WebUIMessage', listener);
                            setTimeout(() => done([toggle.checked, toggle.disabled]));
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
                        document.querySelector('#enable-geolocation').click();
                    """,
                        "args": [],
                    },
                )

            check(switch_state, toggle_switch, settings_file)

            # The Permissions API is exposed to every site, not only under the experimental-interfaces flag.
            command("url", {"url": "about:blank"})
            exposed = command(
                "execute/sync",
                {
                    "script": "return [typeof navigator.permissions, typeof PermissionStatus, typeof Permissions];",
                    "args": [],
                },
            )
            assert exposed == ["object", "function", "function"], exposed
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
    # A profile saved before the location policy existed carries a geolocationEnabled of false that the user never
    # chose, since every save wrote it. Such a profile lets sites ask.
    def check_migrated_profile(switch_state, toggle_switch, settings_file):
        checked, disabled = switch_state()
        assert checked, "a profile with only the old geolocationEnabled key should let sites ask"
        if disabled:
            print("NOTE: no geolocation provider here, so the switch can't be toggled")
            return
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("locationPolicy") == "block", saved
        assert "geolocationEnabled" not in saved, saved
        toggle_switch()
        saved = json.loads(settings_file.read_text())
        assert saved.get("locationPolicy") == "ask", saved

    run_browser(webdriver_binary, {"geolocationEnabled": False}, check_migrated_profile)

    def check_blocking_profile(switch_state, toggle_switch, settings_file):
        checked, _ = switch_state()
        assert not checked, "a profile whose location policy is block should keep sites from asking"

    run_browser(webdriver_binary, {"locationPolicy": "block"}, check_blocking_profile)
    print("PASS: the location policy defaults to letting sites ask, and a saved block stays")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
