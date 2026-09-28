#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import base64
import runpy
import struct
import subprocess
import sys

from pathlib import Path
from urllib.parse import quote

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))


def find_element(port, session, selector):
    status, payload, raw = helpers["request"](
        port,
        "POST",
        f"/session/{session}/element",
        {"using": "css selector", "value": selector},
    )
    assert status == 200, raw
    return next(iter(payload["value"].values()))


def take_element_screenshot(port, session, element):
    return helpers["request"](port, "GET", f"/session/{session}/element/{element}/screenshot")


webdriver = subprocess.Popen(
    [sys.argv[1], "--headless", "-l", "127.0.0.1", "-p", str(port := helpers["unused_port"]())]
)
session = None
try:
    helpers["wait_for_port"](port)
    session = helpers["create_session"](port)

    document = quote(
        "<style>.target{position:absolute;top:0;width:1px;height:1px}</style>"
        '<div id="offscreen" class="target" style="left:-64736px"></div>'
        '<div id="visible" class="target" style="left:0"></div>'
    )
    status, _, raw = helpers["request"](port, "POST", f"/session/{session}/url", {"url": f"data:text/html,{document}"})
    assert status == 200, raw

    offscreen = find_element(port, session, "#offscreen")
    status, payload, raw = take_element_screenshot(port, session, offscreen)
    assert status == 500 and payload["value"]["error"] == "unable to capture screen", raw

    visible = find_element(port, session, "#visible")
    status, payload, raw = take_element_screenshot(port, session, visible)
    assert status == 200, raw
    image = base64.b64decode(payload["value"], validate=True)
    assert image.startswith(b"\x89PNG\r\n\x1a\n")
    assert struct.unpack(">II", image[16:24]) == (1, 1)
finally:
    if session is not None and webdriver.poll() is None:
        helpers["request"](port, "DELETE", f"/session/{session}")
    webdriver.terminate()
    try:
        webdriver.wait(timeout=5)
    except subprocess.TimeoutExpired:
        webdriver.kill()
        webdriver.wait()
