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

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def run_test(webdriver_binary):
    release_download = threading.Event()

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            release_download.wait(30)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"! Test filter list\n")

        def log_message(self, format, *args):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()

    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        (config / "Settings.json").write_text(json.dumps({"contentBlockers": {"enabled": True}}))
        port = helpers["unused_port"]()
        process = subprocess.Popen(
            [webdriver_binary, "--headless", "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
        )
        session = None
        try:
            helpers["wait_for_port"](port)
            session = helpers["create_session"](port)

            def command(path, body):
                status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            def change(name, data):
                return command(
                    "execute/async",
                    {
                        "script": """
                        const [name, data, done] = arguments;
                        document.addEventListener('WebUIMessage', function listener(event) {
                            if (event.detail.name !== 'loadSettings') return;
                            document.removeEventListener('WebUIMessage', listener);
                            done(event.detail.data);
                        });
                        ladybird.sendMessage(name, data);
                    """,
                        "args": [name, data],
                    },
                )

            command("url", {"url": "about:settings#advanced"})
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    function ready() { return document.querySelector('#config-list input[type=number]'); }
                    if (ready()) return done();
                    const observer = new MutationObserver(() => {
                        if (!ready()) return;
                        observer.disconnect();
                        done();
                    });
                    observer.observe(document.documentElement, {childList: true, subtree: true});
                """,
                    "args": [],
                },
            )
            script("""
                window.draftInput = document.querySelector('#config-list input[type=number]');
                draftInput.value = '999';
                draftInput.focus();
                document.querySelector('#dns-server').value = 'unfinished.example';
            """)

            changes = change("setAppearance", {"showBookmarksBar": "never"})
            assert set(changes) == {"appearance"}, changes
            assert changes["appearance"]["showBookmarksBar"] == "never"
            assert script("""
                return draftInput === document.getElementById(draftInput.id)
                    && draftInput === document.activeElement && draftInput.value === '999'
                    && document.querySelector('#dns-server').value === 'unfinished.example';
            """), "An unrelated update replaced an unfinished edit"

            # No enabled lists means no download starts, and no loadSettings message is needed.
            assert command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    const button = document.querySelector('#update-content-blocker-lists');
                    button.click();
                    setTimeout(() => done(!button.disabled && button.textContent === 'Update enabled lists'), 100);
                """,
                    "args": [],
                },
            ), "An update with no eligible lists left the button waiting for a notification"

            variable = script("""
                const input = [...document.querySelectorAll('#config-list input[type=number]')]
                    .find(input => input !== draftInput);
                return {name: input.id, value: Number(input.value) + 1};
            """)
            changes = change("setConfigVariable", variable)
            assert set(changes) == {"configVariables", "configVariableDefinitions"}, changes
            assert script("return draftInput === document.getElementById(draftInput.id) && draftInput.value === '999';")

            # A false boolean must update its own section without clearing any other controls.
            assert change("setGlobalPrivacyControl", True) == {"globalPrivacyControl": True}
            assert change("setGlobalPrivacyControl", False) == {"globalPrivacyControl": False}
            assert script("return !document.querySelector('#global-privacy-control-toggle').checked;")

            engine = {"name": "Test custom engine", "url": "https://example.com/search?q=%s"}
            changes = change("addCustomSearchEngine", engine)
            assert set(changes) == {"searchEngine"}, changes
            assert engine in changes["searchEngine"]["custom"]
            changes = change("removeCustomSearchEngine", engine)
            assert set(changes) == {"searchEngine"}, changes
            assert not changes["searchEngine"].get("custom")

            changes = change("addCustomContentBlockerSubscription", f"http://127.0.0.1:{server.server_port}/list")
            assert set(changes) == {
                "contentBlockers",
                "contentBlockerLists",
                "contentBlockerListUpdateInProgress",
            }, changes
            assert changes["contentBlockerListUpdateInProgress"] is True
            assert script("return document.querySelector('#update-content-blocker-lists').disabled;")
            script("""
                window.finalSettingsChanges = new Promise(resolve => {
                    document.addEventListener('WebUIMessage', function listener(event) {
                        if (event.detail.name !== 'loadSettings') return;
                        if (event.detail.data.contentBlockerListUpdateInProgress !== false) return;
                        document.removeEventListener('WebUIMessage', listener);
                        resolve(event.detail.data);
                    });
                });
            """)
            release_download.set()
            final_changes = command(
                "execute/async",
                {"script": "window.finalSettingsChanges.then(arguments[0]);", "args": []},
            )
            assert set(final_changes) == {"contentBlockerLists", "contentBlockerListUpdateInProgress"}, final_changes
            assert any(item["lastUpdatedAt"] is not None for item in final_changes["contentBlockerLists"])
            assert script("return draftInput === document.getElementById(draftInput.id) && draftInput.value === '999';")

            command("url", {"url": "about:welcome"})
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    const options = document.querySelector('#setup-options');
                    if (!options.disabled) return done();
                    const observer = new MutationObserver(() => {
                        if (options.disabled) return;
                        observer.disconnect();
                        done();
                    });
                    observer.observe(options, {attributes: true, attributeFilter: ['disabled']});
                """,
                    "args": [],
                },
            )
            changes = change("setSearchEngineSettings", {"engine": "Google", "suggestions": True})
            assert set(changes) == {"searchEngine"}, changes
            assert script("""
                return document.querySelector('input[name="search-engine"]:checked')?.value === 'Google'
                    && !document.querySelector('#start-browsing').disabled;
            """), "The welcome page did not apply a partial search setting"
            changes = change("setContentBlockerEnabled", False)
            assert set(changes) == {"contentBlockers"}, changes
            assert script("return !document.querySelector('#enable-content-blocking').checked;")
            print("PASS: settings changes contain only changed fields, preserve edits, and track downloads")
        finally:
            release_download.set()
            if session is not None:
                helpers["request"](port, "DELETE", f"/session/{session}")
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            server.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
