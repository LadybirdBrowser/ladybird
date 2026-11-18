#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import runpy
import subprocess
import tempfile

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

# Clicks a part of the row, and resolves with what that sent to the browser. A click that asks the browser to become
# the default waits for the browser's answer, and tells whether the button was disabled until then and after it.
CLICK = """
    const done = arguments[arguments.length - 1];
    const [target, button] = Array.from(arguments).slice(0, 2).map(selector => document.querySelector(selector));
    const sent = [];
    const sendMessage = ladybird.sendMessage;
    ladybird.sendMessage = (name, data) => {
        sent.push(name);
        sendMessage.call(ladybird, name, data);
    };
    target.click();
    ladybird.sendMessage = sendMessage;

    if (sent.length === 0) return done({sent});
    const waited = button.disabled;
    document.addEventListener('WebUIMessage', function listener(event) {
        if (event.detail.name !== 'defaultBrowserResult') return;
        document.removeEventListener('WebUIMessage', listener);
        done({sent, waited, waiting: button.disabled});
    });
"""

# What a headless browser answers, as it has no UI to ask the system.
FAILURE = "Could not make Ladybird the default browser. Choose it in your system settings instead."


def run_test(webdriver_binary):
    with tempfile.TemporaryDirectory() as directory:
        process, port = helpers["launch_webdriver"](webdriver_binary, "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(path, body):
                status, payload, raw = helpers["request"](port, "POST", f"/session/{session}/{path}", body)
                assert status == 200, raw
                return payload["value"]

            def script(source, *args):
                return command("execute/sync", {"script": source, "args": list(args)})

            # The page handles a message before dispatchEvent returns.
            def message(name, data):
                script(
                    """
                    const [name, data] = arguments;
                    document.dispatchEvent(new CustomEvent('WebUIMessage', {detail: {name, data}}));
                    """,
                    name,
                    data,
                )

            def report(status):
                message("loadSettings", {"defaultBrowser": status})

            def row(selectors):
                return script(
                    """
                    const [group, label, description, button] = Array.from(arguments, id => document.querySelector(id));
                    return {
                        shown: !group.classList.contains('hidden'),
                        text: description.textContent,
                        offersToMakeDefault: !button.classList.contains('hidden'),
                        labelNamesButton: label.htmlFor === button.id,
                        actsAsOneControl: group.classList.contains('control-row'),
                    };
                    """,
                    selectors["group"],
                    selectors["label"],
                    selectors["description"],
                    selectors["button"],
                )

            def click(selectors, target):
                return command("execute/async", {"script": CLICK, "args": [selectors[target], selectors["button"]]})

            settings_row = {
                "group": "#default-browser-group",
                "label": "#default-browser-label",
                "description": "#default-browser-status",
                "button": "#make-default-browser",
            }

            # A headless browser has no UI to ask the system, so the page is told that the status is unavailable.
            command("url", {"url": "about:settings#browsing"})
            command(
                "execute/async",
                {
                    "script": """
                    const done = arguments[0];
                    const group = document.querySelector('#default-browser-group');
                    if (!group.classList.contains('hidden')) return done();
                    const observer = new MutationObserver(() => {
                        if (group.classList.contains('hidden')) return;
                        observer.disconnect();
                        done();
                    });
                    observer.observe(group, {attributes: true, attributeFilter: ['class']});
                    """,
                    "args": [],
                },
            )
            assert row(settings_row) == {
                "shown": True,
                "text": "Ladybird cannot determine or set the default browser on this system.",
                "offersToMakeDefault": False,
                "labelNamesButton": False,
                "actsAsOneControl": False,
            }
            assert click(settings_row, "label") == {"sent": []}

            not_default = {
                "shown": True,
                "text": "Links from other apps open in another browser.",
                "offersToMakeDefault": True,
                "labelNamesButton": True,
                "actsAsOneControl": True,
            }
            report("not-default")
            assert row(settings_row) == not_default

            # The button waits for the browser's answer, and the row says why the browser could not do it.
            asked = {"sent": ["makeDefaultBrowser"], "waited": True, "waiting": False}
            assert click(settings_row, "label") == asked
            assert row(settings_row) == not_default | {"text": FAILURE}

            # A user who turns the system's question down is answered with no failure.
            message("defaultBrowserResult", None)
            assert row(settings_row) == not_default

            assert click(settings_row, "button") == asked
            assert row(settings_row) == not_default | {"text": FAILURE}

            report("default")
            assert row(settings_row) == {
                "shown": True,
                "text": "Links from other apps open in Ladybird.",
                "offersToMakeDefault": False,
                "labelNamesButton": False,
                "actsAsOneControl": False,
            }
            assert click(settings_row, "label") == {"sent": []}

            # A status the browser stops reporting is sent as null.
            report(None)
            assert not row(settings_row)["shown"]

            welcome_row = {
                "group": "#default-browser",
                "label": "#default-browser-label",
                "description": "#default-browser-description",
                "button": "#make-default-browser",
            }

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
            assert not row(welcome_row)["shown"]

            offered = {
                "shown": True,
                "text": "Open links from other apps in Ladybird.",
                "offersToMakeDefault": True,
                "labelNamesButton": True,
                "actsAsOneControl": True,
            }
            report("not-default")
            assert row(welcome_row) == offered

            assert click(welcome_row, "label") == asked
            assert row(welcome_row) == offered | {"text": FAILURE}

            message("defaultBrowserResult", None)
            assert row(welcome_row) == offered

            assert click(welcome_row, "button") == asked
            assert row(welcome_row) == offered | {"text": FAILURE}

            report("default")
            assert row(welcome_row) == {
                "shown": True,
                "text": "Links from other apps open in Ladybird.",
                "offersToMakeDefault": False,
                "labelNamesButton": False,
                "actsAsOneControl": False,
            }
            assert click(welcome_row, "label") == {"sent": []}

            print("PASS: the settings and welcome pages show the default browser status and offer to change it")
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


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)
