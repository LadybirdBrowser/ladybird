#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
# SPDX-License-Identifier: BSD-2-Clause

import os
import plistlib
import subprocess
import sys
import tempfile

from pathlib import Path


def main():
    binary, mode = sys.argv[1:3]
    with tempfile.TemporaryDirectory(prefix="ladybird-native-bindings-") as directory:
        bindings = Path(directory) / "Library" / "KeyBindings"
        bindings.mkdir(parents=True)
        dictionary = {}
        if mode == "custom":
            dictionary = {
                "^w": "deleteWordBackward:",
                "^g": "deleteWordBackward:",
                "^b": "moveWordBackward:",
                "^f": "moveWordForward:",
                "^s": "moveForwardAndModifySelection:",
                "^z": ["moveToBeginningOfParagraph:", "moveForward:"],
                "^u": ["moveBackward:", "transpose:"],
            }
        (bindings / "DefaultKeyBinding.dict").write_bytes(plistlib.dumps(dictionary))
        environment = os.environ.copy()
        environment["CFFIXED_USER_HOME"] = directory
        environment["LADYBIRD_TEST_BINDINGS_MODE"] = mode
        return subprocess.run([binary, *sys.argv[3:]], env=environment, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
