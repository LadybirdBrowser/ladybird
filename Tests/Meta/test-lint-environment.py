#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import os
import runpy
import subprocess
import sys
import tempfile
import threading
import unittest

from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from unittest.mock import Mock
from unittest.mock import patch

SCRIPT = Path(os.environ["LADYBIRD_SOURCE_DIR"]) / "Meta/Linters/run.py"
MODULE = runpy.run_path(str(SCRIPT), run_name="lint_environment")
PREPARE = MODULE["prepare_environment"]


class TestLintEnvironment(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.requirements = self.root / "requirements.txt"
        self.requirements.write_text("ruff==0.15.22\n")
        self.cache = self.root / "cache"
        builder = patch.object(MODULE["venv"], "EnvBuilder")
        self.builder = builder.start()
        self.addCleanup(builder.stop)
        self.builder.return_value.create.side_effect = self.create_environment
        install = patch.object(MODULE["subprocess"], "run")
        self.install = install.start()
        self.addCleanup(install.stop)

    def create_environment(self, environment):
        binaries = environment / ("Scripts" if os.name == "nt" else "bin")
        binaries.mkdir(parents=True, exist_ok=True)
        (binaries / ("python.exe" if os.name == "nt" else "python")).touch()

    def test_reuses_environment_and_installs_requirements_snapshot(self):
        binaries = PREPARE(self.requirements, self.cache)
        self.assertEqual(PREPARE(self.requirements, self.cache), binaries)
        self.install.assert_called_once()
        command = self.install.call_args.args[0]
        self.assertEqual(Path(command[0]).parent, binaries)
        self.assertEqual(Path(command[-1]).read_bytes(), self.requirements.read_bytes())

    def test_requirements_and_interpreter_changes_select_new_environments(self):
        original = PREPARE(self.requirements, self.cache)
        self.requirements.write_text("ruff==0.15.23\n")
        changed_requirements = PREPARE(self.requirements, self.cache)
        with patch.object(sys, "version", "a different interpreter version"):
            changed_python = PREPARE(self.requirements, self.cache)
        self.assertEqual(len({original, changed_requirements, changed_python}), 3)
        self.assertEqual(self.install.call_count, 3)

    def test_failed_installation_is_retried(self):
        self.install.side_effect = subprocess.CalledProcessError(1, ["pip"])
        with self.assertRaises(subprocess.CalledProcessError):
            PREPARE(self.requirements, self.cache)
        self.assertEqual(list(self.cache.glob("*/.ready")), [])
        self.install.side_effect = None
        binaries = PREPARE(self.requirements, self.cache)
        self.assertTrue((binaries.parent / ".ready").is_file())
        self.assertEqual(self.builder.return_value.create.call_count, 2)

    def test_concurrent_setup_installs_once(self):
        barrier = threading.Barrier(2)
        lock_environment = MODULE["lock_environment"]

        def lock_together(lock_file):
            barrier.wait(timeout=10)
            lock_environment(lock_file)

        with patch.dict(PREPARE.__globals__, {"lock_environment": lock_together}):
            with ThreadPoolExecutor(max_workers=2) as executor:
                first = executor.submit(PREPARE, self.requirements, self.cache)
                second = executor.submit(PREPARE, self.requirements, self.cache)
                self.assertEqual(first.result(timeout=10), second.result(timeout=10))
        self.install.assert_called_once()

    def test_tool_arguments_exit_status_and_pyright_version_overrides(self):
        main = MODULE["main"]
        self.install.return_value.returncode = 7
        with patch.dict(main.__globals__, {"prepare_environment": Mock(return_value=self.root)}):
            with patch.object(sys, "argv", ["run.py", "pyright", "a file.py"]):
                with patch.dict(
                    os.environ, {"PYRIGHT_PYTHON_FORCE_VERSION": "latest", "PYRIGHT_PYTHON_PYLANCE_VERSION": "1"}
                ):
                    self.assertEqual(main(), 7)
        command = self.install.call_args.args[0]
        self.assertEqual(Path(command[0]).parent, self.root)
        self.assertEqual(command[1:], ["a file.py"])
        environment = self.install.call_args.kwargs["env"]
        self.assertNotIn("PYRIGHT_PYTHON_FORCE_VERSION", environment)
        self.assertNotIn("PYRIGHT_PYTHON_PYLANCE_VERSION", environment)
        self.assertEqual(environment["PYRIGHT_PYTHON_USE_BUNDLED_PYRIGHT"], "1")


if __name__ == "__main__":
    unittest.main()
