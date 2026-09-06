#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import importlib
import json
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "Meta"))
build_information = importlib.import_module("generate-build-information")


class BuildInformationTests(unittest.TestCase):
    def test_flags_exclude_private_paths_and_string_definitions(self):
        command = '/Users/private/compiler -I /Users/private/include -isysroot /Users/private/sdk -DUSER_NAME="private" -fdebug-prefix-map=/Users/private=secret -Xclang private -O2 -g1 -std=c++23 -DNDEBUG -DENABLE_FEATURE=1 -fsanitize=address,undefined -march=native -o /Users/private/object -c /Users/private/source.cpp'
        flags = build_information.compiler_flags(command)
        self.assertNotIn("private", flags)
        self.assertNotIn("secret", flags)
        self.assertIn("-O2", flags)
        self.assertIn("-fsanitize=address,undefined", flags)
        self.assertIn("-DENABLE_FEATURE=1", flags)
        self.assertIn("-march=native", flags)

    def test_revision_tracks_commits_and_modified_sources(self):
        with tempfile.TemporaryDirectory(prefix="ladybird-build-information-") as temporary:
            source = Path(temporary) / "source"
            build = Path(temporary) / "build"
            source.mkdir()
            build.mkdir()
            (build / "CMakeCache.txt").write_text(
                "ENABLE_ADDRESS_SANITIZER:BOOL=ON\nPRIVATE_PATH:PATH=/Users/private\n"
            )
            (build / "compile_commands.json").write_text(
                json.dumps(
                    [
                        {
                            "file": str(source / "Services/WebContent/main.cpp"),
                            "command": "c++ -O2 -DNDEBUG -c main.cpp",
                        }
                    ]
                )
            )
            self.assertIn("Git commit: unknown", build_information.generate(source, build, "Clang 21.0"))

            def git(*arguments):
                return subprocess.check_output(
                    [
                        "git",
                        "-C",
                        str(source),
                        "-c",
                        "user.name=Test",
                        "-c",
                        "user.email=test@example.invalid",
                        *arguments,
                    ],
                    stderr=subprocess.DEVNULL,
                    text=True,
                ).strip()

            git("init")
            tracked = source / "tracked.txt"
            tracked.write_text("first\n")
            git("add", "tracked.txt")
            git("commit", "-m", "First")
            first = git("rev-parse", "HEAD")
            info = build_information.generate(source, build, "Clang 21.0")
            self.assertIn(f"Git commit: {first}", info)
            self.assertIn("Tracked source state: clean", info)
            self.assertIn("ENABLE_ADDRESS_SANITIZER=ON", info)
            self.assertIn("C++ compiler: Clang 21.0", info)
            self.assertNotIn("private", info)
            tracked.write_text("second\n")
            self.assertIn("Tracked source state: modified", build_information.generate(source, build, "Clang 21.0"))
            git("commit", "-am", "Second")
            second = git("rev-parse", "HEAD")
            self.assertNotEqual(first, second)
            self.assertIn(f"Git commit: {second}", build_information.generate(source, build, "Clang 21.0"))


if __name__ == "__main__":
    unittest.main()
