#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# https://w3c.github.io/webdriver/#dfn-element-send-keys
# A file input selects the files Element Send Keys names, which the renderer has the UI process open one at a time for
# the command. Any other element just takes the text, and a file the renderer can't read is an invalid argument.

import runpy
import subprocess
import sys
import tempfile
import time

from pathlib import Path

helpers = runpy.run_path(str(Path(__file__).with_name("test-webdriver-delete-session.py")))

PAGE = "data:text/html,<input id=text><input id=file type=file multiple>"


def main():
    with tempfile.TemporaryDirectory() as directory:
        file_path = Path(directory) / "file.txt"
        file_path.write_text("content")

        process, port = helpers["launch_webdriver"](sys.argv[1], "--headless", "--profile-path", directory)
        session = None
        try:
            session = helpers["create_session"](port)

            def command(method, path, body=None):
                return helpers["request"](port, method, f"/session/{session}/{path}", body)

            def value_of(method, path, body=None):
                status, payload, raw = command(method, path, body)
                assert status == 200, raw
                return payload["value"]

            def script(source):
                return value_of("POST", "execute/sync", {"script": source, "args": []})

            value_of("POST", "url", {"url": PAGE})
            deadline = time.monotonic() + 30
            while script("return document.readyState") != "complete":
                assert time.monotonic() < deadline, "The page did not finish loading"
                time.sleep(0.01)

            def element(selector):
                found = value_of("POST", "element", {"using": "css selector", "value": selector})
                return next(iter(found.values()))

            text_input = element("#text")
            file_input = element("#file")

            value_of("POST", f"element/{text_input}/value", {"text": str(file_path)})
            assert script("return document.getElementById('text').value") == str(file_path)
            print("PASS: a text input takes a file's path as text")

            value_of("POST", f"element/{file_input}/value", {"text": str(file_path)})
            files = script("return Array.from(document.getElementById('file').files, file => [file.name, file.size])")
            assert files == [["file.txt", 7]], files
            print("PASS: a file input selects the file")

            for path, reason in ((Path(directory) / "missing.txt", "a missing file"), (Path(directory), "a directory")):
                status, payload, raw = command("POST", f"element/{file_input}/value", {"text": str(path)})
                assert status == 400 and payload["value"]["error"] == "invalid argument", raw
                print(f"PASS: {reason} is an invalid argument")

            # The renderer is still there to run commands.
            assert script("return 1 + 1") == 2
            print("PASS: the renderer survives a file it couldn't read")
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
    main()
