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
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(json.dumps({"firstRunComplete": True}))
        permissions_file = config / "Permissions.json"
        permissions_file.write_text(
            json.dumps(
                {
                    "version": 1,
                    "decisions": [
                        {"origin": "https://example.com", "permission": "geolocation", "decision": "allow"},
                        {"origin": "https://talk.example.org", "permission": "microphone", "decision": "block"},
                    ],
                    "embargoes": [],
                }
            )
        )

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

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            def rows(hash, permission):
                command("url", {"url": f"about:settings#{hash}"})
                return command(
                    "execute/async",
                    {
                        "script": """
                        const permission = arguments[0];
                        const done = arguments[1];
                        const dialog = document.querySelector(`#site-permissions-${permission}`);
                        const read = () => Array.from(dialog.querySelectorAll('.site-permission-row'))
                            .map(row => [row.dataset.origin, row.querySelector('select').value]);
                        const list = dialog.querySelector('.site-permissions-list');
                        if (read().length) return done(read());
                        const observer = new MutationObserver(() => {
                            if (!read().length) return;
                            observer.disconnect();
                            done(read());
                        });
                        observer.observe(list, { childList: true, subtree: true });
                    """,
                        "args": [permission],
                    },
                )

            def saved_decisions():
                deadline = time.time() + 5
                while True:
                    # The browser rewrites the file in place, so a read may overlap a write and see part of it.
                    try:
                        decisions = json.loads(permissions_file.read_text())["decisions"]
                    except (OSError, ValueError, KeyError):
                        decisions = None
                    if decisions is not None:
                        yield decisions
                    if time.time() > deadline:
                        return
                    time.sleep(0.1)

            def wait_for_saved(condition):
                for decisions in saved_decisions():
                    if condition(decisions):
                        return decisions
                raise AssertionError(f"Permissions.json never matched: {permissions_file.read_text()}")

            assert rows("location-permissions", "geolocation") == [["https://example.com", "allow"]]
            assert rows("microphone-permissions", "microphone") == [["https://talk.example.org", "block"]]

            rows("location-permissions", "geolocation")
            script("""
                const select = document.querySelector('#site-permissions-geolocation .site-permission-row select');
                select.value = 'block';
                select.dispatchEvent(new Event('change'));
            """)
            wait_for_saved(
                lambda decisions: (
                    {"origin": "https://example.com", "permission": "geolocation", "decision": "block"} in decisions
                )
            )

            script("document.querySelector('#site-permissions-geolocation .site-permission-row button').click();")
            wait_for_saved(lambda decisions: all(entry["origin"] != "https://example.com" for entry in decisions))

            print("PASS: about:settings lists, changes, and removes site permissions")
        finally:
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
