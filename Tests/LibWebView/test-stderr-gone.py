#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# A Ladybird whose stderr is gone (here, a pipe whose reader has exited, so that every write to it fails) keeps
# running: Its processes ignore SIGPIPE, and their Rust code drops a log line that it can't write. The page has the
# Rust bindings log a FIXME, and has the C++ side log a console message.

import argparse
import os
import signal
import subprocess
import tempfile

from pathlib import Path

PAGE = """<!doctype html>
<p id=out>started</p>
<script>
navigator.credentials.preventSilentAccess().then(() => {}, () => {
    console.log("logged with stderr gone");
    document.getElementById("out").textContent = "settled with stderr gone";
});
</script>
"""

TIMEOUT_SECONDS = 60


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ladybird_binary")
    ladybird_binary = parser.parse_args().ladybird_binary

    with tempfile.TemporaryDirectory(prefix="ladybird-stderr-gone-") as directory:
        page = Path(directory) / "page.html"
        page.write_text(PAGE)
        profile = Path(directory) / "profile"

        reader, writer = os.pipe()
        os.close(reader)
        # The browser starts with SIGPIPE at its default, as from a shell, since subprocess restores it.
        process = subprocess.Popen(
            [ladybird_binary, "--headless=text", f"--profile-path={profile}", page.as_uri()],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=writer,
            start_new_session=True,
            text=True,
        )
        os.close(writer)
        try:
            output, _ = process.communicate(timeout=TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
            raise AssertionError(f"The browser did not finish within {TIMEOUT_SECONDS} seconds") from None
        finally:
            # Take down any helper process that outlived the browser.
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    assert process.returncode == 0, f"The browser exited with {process.returncode}"
    assert "settled with stderr gone" in output, f"The page did not finish: {output!r}"
    print("PASS: the browser runs on with its stderr gone")


if __name__ == "__main__":
    main()
