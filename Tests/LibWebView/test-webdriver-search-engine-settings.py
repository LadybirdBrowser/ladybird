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


def run_test(webdriver_binary, graphical):
    with tempfile.TemporaryDirectory() as directory:
        config = Path(directory) / "config"
        config.mkdir()
        settings_file = config / "Settings.json"
        settings_file.write_text(
            json.dumps(
                {
                    "searchEngine": {
                        "name": "Google",
                        "custom": [{"name": "Plain custom", "url": "https://example.com/search?q=%s"}],
                    },
                    "autocompleteEngine": {"name": "Google"},
                }
            )
        )
        port = helpers["unused_port"]()
        arguments = [webdriver_binary, "-l", "127.0.0.1", "-p", str(port), "--profile-path", directory]
        if not graphical:
            arguments.append("--headless")
        process = subprocess.Popen(arguments)
        session = None
        try:
            helpers["wait_for_port"](port)
            status, payload, raw = helpers["request"](
                port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": not graphical}}}
            )
            assert status == 200, raw
            session = payload["value"]["sessionId"]

            def command(path, body=None, method="POST"):
                status, payload, raw = helpers["request"](port, method, f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            def change_settings(action, condition, *args):
                command(
                    "execute/async",
                    {
                        "script": f"""
                        const parameters = Array.from(arguments);
                        const done = parameters.pop();
                        document.addEventListener('WebUIMessage', function listener(event) {{
                            if (event.detail.name !== 'loadSettings') return;
                            const settings = event.detail.data;
                            if (!({condition})) return;
                            document.removeEventListener('WebUIMessage', listener);
                            done();
                        }});
                        {action}
                    """,
                        "args": list(args),
                    },
                )

            def select_engine(name):
                change_settings(
                    """
                    const select = document.querySelector('#search-engine');
                    select.value = parameters[0];
                    select.dispatchEvent(new Event('change'));
                """,
                    "(settings.searchEngine.engine || '') === parameters[0]",
                    name,
                )

            def toggle_suggestions(enabled):
                change_settings(
                    "document.querySelector('#search-suggestions-enabled').click();",
                    "settings.searchEngine.suggestions === parameters[0]",
                    enabled,
                )

            def state():
                return script("""
                    const checkbox = document.querySelector('#search-suggestions-enabled');
                    return [checkbox.checked, checkbox.disabled,
                            document.querySelector('#search-suggestions-description').textContent];
                """)

            def load_settings(expected_engine):
                command("url", {"url": "about:settings#search"})
                command(
                    "execute/async",
                    {
                        "script": """
                        const expectedEngine = arguments[0];
                        const done = arguments[1];
                        const select = document.querySelector('#search-engine');
                        function ready() { return select.value === expectedEngine && select.options.length > 10; }
                        if (ready()) return done();
                        const observer = new MutationObserver(() => {
                            if (!ready()) return;
                            observer.disconnect();
                            done();
                        });
                        observer.observe(select, {childList: true});
                    """,
                        "args": [expected_engine],
                    },
                )

            load_settings("Google")
            assert state() == [True, False, "Sends what you type to Google."]
            for engine in ("Bing", "Ecosia", "Yandex"):
                select_engine(engine)
                assert state() == [True, False, f"Sends what you type to {engine}."]
            select_engine("Mojeek")
            assert state() == [True, True, "Search suggestions aren't available for Mojeek."]
            select_engine("DuckDuckGo")
            assert state() == [True, False, "Sends what you type to DuckDuckGo."]
            select_engine("Brave")
            assert state() == [True, False, "Sends what you type to Brave."]
            toggle_suggestions(False)
            select_engine("Google")
            assert state()[:2] == [False, False]
            select_engine("Plain custom")
            assert state()[:2] == [False, True]

            script("document.querySelector('#search-settings').click()")
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    const dialog = document.querySelector('#search-dialog');
                    if (dialog.open) return done();
                    const observer = new MutationObserver(() => {
                        if (!dialog.open) return;
                        observer.disconnect();
                        done();
                    });
                    observer.observe(dialog, {attributes: true, attributeFilter: ['open']});
                """,
                    "args": [],
                },
            )
            script("""
                document.querySelector('#search-custom-name').value = 'Custom suggestions';
                document.querySelector('#search-custom-url').value = 'https://example.com/search?q=%s';
            """)
            for url in ("https://example.com/suggest", "file:///tmp/%s", "data:text/plain,%s"):
                script(
                    """
                    document.querySelector('#search-custom-suggestions-url').value = arguments[0];
                    document.querySelector('#search-custom-add').click();
                """,
                    url,
                )
                assert script(
                    "return document.querySelector('#search-custom-suggestions-url').classList.contains('error')"
                )

            change_settings(
                """
                document.querySelector('#search-custom-suggestions-url').value = 'https://example.com/suggest?q=%s';
                document.querySelector('#search-custom-add').click();
            """,
                "settings.searchEngine.custom.some(engine => engine.name === 'Custom suggestions')",
            )
            script("document.querySelector('#search-close').click()")
            select_engine("Custom suggestions")
            assert state()[:2] == [False, False]
            toggle_suggestions(True)
            saved = json.loads(settings_file.read_text())
            assert "autocompleteEngine" not in saved
            assert saved["searchEngine"]["engine"] == "Custom suggestions"
            assert saved["searchEngine"]["suggestions"]
            assert "name" not in saved["searchEngine"]
            assert saved["searchEngine"]["custom"][1]["suggestionsUrl"] == "https://example.com/suggest?q=%s"

            helpers["request"](port, "DELETE", f"/session/{session}")
            session = helpers["create_session"](port)
            load_settings("Custom suggestions")
            assert state()[:2] == [True, False], "Combined search preference did not survive a new browser process"
            change_settings(
                """
                ladybird.sendMessage('removeCustomSearchEngine', {
                    name: 'Custom suggestions', url: 'https://example.com/search?q=%s'
                });
            """,
                "!settings.searchEngine.engine",
            )
            assert state() == [True, True, "Choose a search engine to use search suggestions."]
            print("PASS: search and suggestions use one engine, migrate, and preserve the user's toggle")
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
    parser.add_argument("--graphical", action="store_true")
    args = parser.parse_args()
    run_test(args.webdriver_binary, args.graphical)
