#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import importlib
import os
import subprocess
import urllib.parse

webdriver = importlib.import_module("test-webdriver-delete-session")

PAGE = """<!doctype html>
<style>html, body { margin: 0; } button { position: absolute; left: 0; top: 0; width: 200px; height: 100px; }</style>
<button onclick="window.clicks = (window.clicks || 0) + 1">Click</button>
"""


def click(port, session_id):
    status, _payload, response = webdriver.request(
        port,
        "POST",
        f"/session/{session_id}/actions",
        {
            "actions": [
                {
                    "type": "pointer",
                    "id": "mouse",
                    "parameters": {"pointerType": "mouse"},
                    "actions": [
                        {"type": "pointerMove", "origin": "viewport", "x": 100, "y": 50},
                        {"type": "pointerDown", "button": 0},
                        {"type": "pointerUp", "button": 0},
                    ],
                }
            ]
        },
    )
    assert status == 200, response


def clicks(port, session_id):
    status, payload, response = webdriver.request(
        port, "POST", f"/session/{session_id}/execute/sync", {"script": "return window.clicks ?? 0;", "args": []}
    )
    assert status == 200, response
    return payload["value"]


def run_test(webdriver_binary):
    # A graphical session runs the browser's UI, which hands its own input to the Hub. Pointer actions are input the Hub
    # comes up with itself, which the Hub must not take for the UI's. The offscreen platform lets the UI run with no
    # display, as on CI, unless the environment names another.
    environment = os.environ.copy()
    environment.setdefault("QT_QPA_PLATFORM", "offscreen")
    process, port = webdriver.launch_webdriver(webdriver_binary, env=environment)
    try:
        status, payload, response = webdriver.request(
            port, "POST", "/session", {"capabilities": {"alwaysMatch": {"ladybird:headless": False}}}
        )
        assert status == 200, response
        session_id = payload["value"]["sessionId"]

        url = "data:text/html," + urllib.parse.quote(PAGE)
        status, _payload, response = webdriver.request(port, "POST", f"/session/{session_id}/url", {"url": url})
        assert status == 200, response

        click(port, session_id)
        click(port, session_id)
        assert clicks(port, session_id) == 2

        status, _payload, response = webdriver.request(port, "DELETE", f"/session/{session_id}")
        assert status == 200, response
        print("PASS: Pointer actions in a graphical session reach the page")
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("webdriver_binary")
    run_test(parser.parse_args().webdriver_binary)


if __name__ == "__main__":
    main()
