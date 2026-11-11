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
import tempfile
import threading
import time

from contextlib import contextmanager
from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PERMISSIONS = [
    ("geolocation", "Location", "https://example.com", "allow"),
    ("microphone", "Microphone", "https://talk.example.org", "block"),
    ("notifications", "Notifications", "https://news.example.net", "allow"),
]


@contextmanager
def run_browser(webdriver_binary, settings, decisions=()):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(json.dumps({"firstRunComplete": True, **settings}))
        permissions_file = config / "Permissions.json"
        permissions_file.write_text(
            json.dumps(
                {
                    "version": 1,
                    "decisions": list(decisions),
                    "embargoes": [],
                }
            )
        )

        process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(path, body=None, method="POST"):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            yield command, script, settings_file, permissions_file
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


def wait_for_saved(path, condition):
    deadline = time.monotonic() + 5
    saved = None
    while True:
        # The browser rewrites the file in place, so a read may overlap a write and see part of it.
        try:
            saved = json.loads(path.read_text())
        except (OSError, ValueError):
            pass
        else:
            if condition(saved):
                return saved
        if time.monotonic() >= deadline:
            raise AssertionError(f"{path.name} never matched: {saved}")
        time.sleep(0.1)


def check_policies(webdriver_binary, site_url):
    for initial_policy in ("ask", "block"):
        # Old profiles wrote geolocationEnabled=false even when the user never chose it. They should let sites ask.
        settings = (
            {"geolocationEnabled": False}
            if initial_policy == "ask"
            else {permission: {"policy": "block"} for permission, *_ in PERMISSIONS}
        )
        with run_browser(webdriver_binary, settings) as (command, script, settings_file, _):
            # The Permissions API is exposed without the experimental-interfaces flag, even on about:blank.
            command("url", {"url": "about:blank"})
            assert script("return [typeof navigator.permissions, typeof PermissionStatus, typeof Permissions];") == [
                "object",
                "function",
                "function",
            ]

            def switch_state(permission):
                command("url", {"url": "about:settings#permissions"})
                return command(
                    "execute/async",
                    {
                        "script": """
                        const [permission, done] = arguments;
                        const pending = new Set(['loadSettings', 'loadFeatures']);
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (!pending.delete(event.detail.name) || pending.size) return;
                            document.removeEventListener('WebUIMessage', listener);
                            setTimeout(() => {
                                const toggle = document.querySelector(`#${permission}-policy`);
                                done([toggle.checked, toggle.disabled]);
                            });
                        });
                        // Request both again in case the page already received its initial settings and features.
                        ladybird.sendMessage('loadCurrentSettings');
                        ladybird.sendMessage('loadFeatures');
                    """,
                        "args": [permission],
                    },
                )

            policies = {permission: initial_policy for permission, *_ in PERMISSIONS}

            def check_site_permissions(policies):
                command("url", {"url": site_url})
                states = command(
                    "execute/async",
                    {
                        "script": """
                        const [permissions, done] = arguments;
                        Promise.all(permissions.map(name => navigator.permissions.query({ name })
                            .then(status => status.state))).then(done);
                    """,
                        "args": [list(policies)],
                    },
                )
                expected = ["prompt" if policy == "ask" else "denied" for policy in policies.values()]
                assert states == expected, (policies, states)

            check_site_permissions(policies)
            for permission, *_ in PERMISSIONS:
                checked, disabled = switch_state(permission)
                assert checked == (initial_policy == "ask"), (permission, initial_policy, checked)
                if disabled:
                    assert permission == "geolocation", f"Unexpectedly disabled policy: {permission}"
                    print("NOTE: no geolocation provider here, so the location switch can't be toggled")
                    continue

                # Exercise both directions and check that other permissions retain their policies.
                for policy in ("block" if initial_policy == "ask" else "ask", initial_policy):
                    script("document.querySelector(`#${arguments[0]}-policy`).click();", permission)
                    policies[permission] = policy
                    saved = wait_for_saved(
                        settings_file,
                        lambda saved, policies=policies: all(
                            saved.get(name) == {"policy": value} for name, value in policies.items()
                        ),
                    )
                    assert "geolocationEnabled" not in saved, saved
                    check_site_permissions(policies)
                    checked, _ = switch_state(permission)
                    assert checked == (policy == "ask"), (permission, policy, checked)

    print(
        "PASS: permission policies default to ask, load saved blocks, and persist changes reflected by the Permissions API"
    )


def check_site_settings(webdriver_binary):
    settings = {
        "autoplay": {
            "policy": "block-audio",
            "siteFilters": [{"filter": "https://media.example", "policy": "allow-audio-and-video"}],
        },
    }
    decisions = [
        {"origin": origin, "permission": permission, "decision": decision}
        for permission, _, origin, decision in PERMISSIONS
    ]
    with run_browser(webdriver_binary, settings, decisions) as (command, script, settings_file, permissions_file):

        def wait_for_rows(expected, title):
            return command(
                "execute/async",
                {
                    "script": """
                    const [expected, title, done] = arguments;
                    const dialog = document.querySelector('#site-settings');
                    const read = () => Array.from(dialog.querySelectorAll('.dialog-list-item'))
                        .map(row => [row.dataset.filter, row.querySelector('select')?.value ?? null]).sort();
                    const ready = () => dialog.open
                        && document.querySelector('#site-settings-title').textContent === title
                        && JSON.stringify(read()) === JSON.stringify(expected);
                    if (ready()) return done(read());
                    const observer = new MutationObserver(() => {
                        if (!ready()) return;
                        observer.disconnect();
                        done(read());
                    });
                    observer.observe(dialog, { childList: true, subtree: true, attributes: true });
                """,
                    "args": [sorted(expected), title],
                },
            )

        def open_settings(setting, expected, title):
            command("url", {"url": f"about:settings#{setting}"})
            return wait_for_rows(expected, title)

        def add_site(origin, decision="allow"):
            script(
                """
                const input = document.querySelector('#site-settings-input');
                input.value = arguments[0];
                input.dispatchEvent(new Event('input'));
                document.querySelector('#site-settings-decision').value = arguments[1];
                document.querySelector('#site-settings-add').click();
                """,
                origin,
                decision,
            )

        def wait_for_decisions(condition):
            return wait_for_saved(permissions_file, lambda saved: condition(saved["decisions"]))["decisions"]

        def check_invalid_urls():
            for value in ("not a URL", "https://", "about:blank", "file:///tmp/test", "data:text/html,test"):
                add_site(value)
                assert script("return !document.querySelector('#site-settings-input').validity.valid;"), value

        for index, (permission, title, origin, decision) in enumerate(PERMISSIONS):
            open_settings(permission, [[origin, decision]], title)

            # Invalid origins stay in the form; valid URLs are stored as origins, without paths or queries.
            check_invalid_urls()
            add_site("localhost:3000/path?query", "block")
            wait_for_decisions(
                lambda decisions, permission=permission: (
                    {
                        "origin": "https://localhost:3000",
                        "permission": permission,
                        "decision": "block",
                    }
                    in decisions
                )
            )
            wait_for_rows([[origin, decision], ["https://localhost:3000", "block"]], title)

            script("""
                const row = document.querySelector('[data-filter="https://localhost:3000"]');
                const select = row.querySelector('select');
                select.value = 'allow';
                select.dispatchEvent(new Event('change'));
            """)
            wait_for_decisions(
                lambda decisions, permission=permission: (
                    {
                        "origin": "https://localhost:3000",
                        "permission": permission,
                        "decision": "allow",
                    }
                    in decisions
                )
            )
            wait_for_rows([[origin, decision], ["https://localhost:3000", "allow"]], title)

            script("document.querySelector('[data-filter=\"https://localhost:3000\"] button').click();")
            wait_for_decisions(
                lambda decisions: all(entry["origin"] != "https://localhost:3000" for entry in decisions)
            )
            wait_for_rows([[origin, decision]], title)

            script("document.querySelector('#site-settings-remove-all').click();")
            remaining = wait_for_decisions(
                lambda decisions, permission=permission: all(entry["permission"] != permission for entry in decisions)
            )
            wait_for_rows([], title)
            # Removing every site for one permission leaves the other permissions alone.
            later = PERMISSIONS[index + 1 :]
            for other, _, other_origin, other_decision in later:
                assert {"origin": other_origin, "permission": other, "decision": other_decision} in remaining

        # Every autoplay level can be added and edited.
        open_settings("autoplay", [["https://media.example", "allow-audio-and-video"]], "Autoplay")
        check_invalid_urls()
        assert script("""
            return Array.from(document.querySelector('#site-settings-decision').options, option => option.value);
        """) == ["allow-audio-and-video", "block-audio", "block-audio-and-video"]
        add_site("  quiet.example:8443/path  ", "block-audio")
        wait_for_rows(
            [["https://media.example", "allow-audio-and-video"], ["https://quiet.example:8443", "block-audio"]],
            "Autoplay",
        )
        script("""
            const select = document.querySelector('[data-filter="https://media.example"] select');
            select.value = 'block-audio-and-video';
            select.dispatchEvent(new Event('change'));
        """)
        wait_for_rows(
            [["https://media.example", "block-audio-and-video"], ["https://quiet.example:8443", "block-audio"]],
            "Autoplay",
        )
        wait_for_saved(
            settings_file,
            lambda saved: (
                sorted(saved["autoplay"]["siteFilters"], key=lambda site: site["filter"])
                == [
                    {"filter": "https://media.example", "policy": "block-audio-and-video"},
                    {"filter": "https://quiet.example:8443", "policy": "block-audio"},
                ]
            ),
        )
        # Reloading the page must retain the chosen policies.
        open_settings(
            "autoplay",
            [["https://media.example", "block-audio-and-video"], ["https://quiet.example:8443", "block-audio"]],
            "Autoplay",
        )
        script("document.querySelector('[data-filter=\"https://quiet.example:8443\"] button').click();")
        wait_for_rows([["https://media.example", "block-audio-and-video"]], "Autoplay")
        script("document.querySelector('#site-settings-remove-all').click();")
        wait_for_rows([], "Autoplay")
        wait_for_saved(settings_file, lambda saved: saved["autoplay"]["siteFilters"] == [])

        # Changing hashes can switch the contents of an already open shared dialog.
        script("location.hash = 'microphone';")
        wait_for_rows([], "Microphone")
        script("location.hash = 'notifications';")
        wait_for_rows([], "Notifications")
        command("back", {})
        wait_for_rows([], "Microphone")
        command("forward", {})
        wait_for_rows([], "Notifications")
        script("document.querySelector('#site-settings-close').click();")
        wait_for_rows([], "Microphone")

        print("PASS: shared site settings add, change, remove, and clear permissions and autoplay exceptions")


def run_test(webdriver_binary):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.end_headers()
            self.wfile.write(b"<!doctype html><title>Permissions test</title>")

        def log_message(self, format, *args):
            pass

    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            check_policies(webdriver_binary, f"http://127.0.0.1:{server.server_port}/")
        finally:
            server.shutdown()
    check_site_settings(webdriver_binary)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    args = parser.parse_args()
    run_test(args.webdriver_binary)
