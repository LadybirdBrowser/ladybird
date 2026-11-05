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
LINTER = runpy.run_path(str(SCRIPT), run_name="lint_environment")
MODULE = runpy.run_path(str(SCRIPT.parent.parent / "Utils/python_environment.py"), run_name="python_environment")
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
        with patch("builtins.print") as output:
            binaries = PREPARE(self.requirements, self.cache)
            self.assertEqual(PREPARE(self.requirements, self.cache), binaries)
        output.assert_not_called()
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

    def test_missing_interpreter_is_recreated(self):
        binaries = PREPARE(self.requirements, self.cache)
        (binaries / ("python.exe" if os.name == "nt" else "python")).unlink()
        self.assertEqual(PREPARE(self.requirements, self.cache), binaries)
        self.assertEqual(self.install.call_count, 2)

    def test_running_from_virtual_environment_uses_same_cache(self):
        original = PREPARE(self.requirements, self.cache)
        base = getattr(sys, "_base_executable", sys.executable)
        with patch.object(sys, "executable", str(original / "python")):
            with patch.object(sys, "_base_executable", base, create=True):
                self.assertEqual(PREPARE(self.requirements, self.cache), original)
        self.install.assert_called_once()

    def test_interpreter_aliases_use_same_cache(self):
        interpreter = self.root / "python"
        interpreter.touch()
        alias = self.root / "python3"
        alias.symlink_to(interpreter)
        with patch.object(sys, "_base_executable", str(interpreter), create=True):
            original = PREPARE(self.requirements, self.cache)
        with patch.object(sys, "_base_executable", str(alias), create=True):
            self.assertEqual(PREPARE(self.requirements, self.cache), original)
        self.install.assert_called_once()

    def test_printing_interpreter_path_does_not_prepare_environment(self):
        main = MODULE["main"]
        with patch.dict(main.__globals__, {"REQUIREMENTS": self.requirements, "CACHE_ROOT": self.cache}):
            with patch.object(sys, "argv", ["python_environment.py", "--print-python"]):
                with patch("builtins.print") as output:
                    main()
        self.assertFalse(self.cache.exists())
        self.builder.assert_not_called()
        self.install.assert_not_called()
        binaries = PREPARE(self.requirements, self.cache)
        self.assertEqual(
            output.call_args.args[0], (binaries / ("python.exe" if os.name == "nt" else "python")).as_posix()
        )

    def test_setup_is_quiet_unless_binary_directory_is_requested(self):
        main = MODULE["main"]
        prepare = Mock(return_value=self.root)
        with patch.dict(main.__globals__, {"prepare_environment": prepare}):
            with patch.object(sys, "argv", ["python_environment.py"]):
                with patch("builtins.print") as output:
                    main()
            output.assert_not_called()
            prepare.assert_called_once()
            with patch.object(sys, "argv", ["python_environment.py", "--print-bin"]):
                with patch("builtins.print") as output:
                    main()
            output.assert_called_once_with(self.root.as_posix())
            self.assertEqual(prepare.call_count, 2)

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
        main = LINTER["main"]
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
