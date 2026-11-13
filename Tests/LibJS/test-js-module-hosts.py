#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import argparse
import json
import subprocess
import sys
import tempfile
import unittest

from pathlib import Path
from typing import Optional

REJECT_AFTER_WAIT = """
globalThis.view = new Int32Array(new SharedArrayBuffer(8));
globalThis.pending = Atomics.waitAsync(view, 0, 0).value;
await Atomics.waitAsync(view, 1, 0, 1).value;
throw new Error("module rejection sentinel");
"""


class ModuleHostTests(unittest.TestCase):
    js: Path
    test262_runner: Optional[Path]

    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="libjs-module-host-")
        self.addCleanup(directory.cleanup)
        self.directory = Path(directory.name)

    def run_process(self, command, **kwargs):
        try:
            return subprocess.run(command, capture_output=True, text=True, timeout=30, **kwargs)
        except subprocess.TimeoutExpired as error:
            self.fail(f"Module execution did not finish: {error}")

    def run_cli_module(self, source):
        path = self.directory / "module.mjs"
        path.write_text(source, encoding="utf-8")
        return self.run_process([str(self.js), "--as-module", str(path)])

    def run_test262_module(self, source, negative=False, flags="module, raw"):
        if self.test262_runner is None:
            self.skipTest("test262-runner is not built on this platform")
        metadata = f"flags: [{flags}]\n"
        if negative:
            metadata += "negative:\n  phase: runtime\n  type: Error\n"
        path = self.directory / "module.js"
        path.write_text(f"/*---\n{metadata}---*/\n{source}", encoding="utf-8")
        process = self.run_process(
            [str(self.test262_runner), "--harness-location", str(self.directory)], input=f"{path}\n"
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        results = [
            json.loads(line.removeprefix("RESULT ").rstrip("\0"))
            for line in process.stdout.splitlines()
            if line.startswith("RESULT ")
        ]
        self.assertEqual(len(results), 1, process.stdout)
        return results[0]

    def test_cli_rejection_stops_an_unrelated_infinite_wait(self):
        process = self.run_cli_module(REJECT_AFTER_WAIT)
        self.assertEqual(process.returncode, 1, process.stderr)
        self.assertIn("module rejection sentinel", process.stderr)

    def test_cli_fulfilled_module_still_drains_waits(self):
        process = self.run_cli_module("""
globalThis.view = new Int32Array(new SharedArrayBuffer(4));
Atomics.waitAsync(view, 0, 0, 1).value.then(() => print("wait drained"));
""")
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("wait drained", process.stdout)

    def test_cli_pending_module_fails(self):
        for prefix in ["", "await Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value;"]:
            with self.subTest(prefix=prefix):
                process = self.run_cli_module(prefix + "\nawait new Promise(() => {});")
                self.assertEqual(process.returncode, 1, process.stderr)
                self.assertIn("Module evaluation did not complete", process.stderr)

    @unittest.skipIf(sys.platform == "win32", "The REPL is not built on this platform")
    def test_repl_pending_module_can_complete_in_a_later_input(self):
        process = self.run_process(
            [str(self.js), "--as-module"],
            input='await new Promise(resolve => globalThis.finish = resolve); print("module completed");\nfinish();\n',
        )
        self.assertEqual(process.returncode, 0, process.stderr)
        self.assertIn("module completed", process.stdout)
        self.assertNotIn("Module evaluation did not complete", process.stderr)

    def test_runner_pending_module_cannot_pass(self):
        for prefix in ["", "await Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value;"]:
            with self.subTest(prefix=prefix):
                result = self.run_test262_module(prefix + "\nawait new Promise(() => {});")
                self.assertNotEqual(result["result"], "passed", result)

    def test_runner_pending_module_cannot_match_an_expected_exception(self):
        result = self.run_test262_module("await new Promise(() => {});", negative=True)
        self.assertNotEqual(result["result"], "passed", result)

    def test_runner_rejection_matches_an_expected_exception(self):
        result = self.run_test262_module(REJECT_AFTER_WAIT, negative=True)
        self.assertEqual(result["result"], "passed", result)

    def test_runner_fulfilled_module_passes(self):
        result = self.run_test262_module("""
await Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value;
print("module completed");
""")
        self.assertEqual(result["result"], "passed", result)
        self.assertIn("module completed", result["output"])

    def test_runner_timer_polling_allows_async_wait_timeouts(self):
        result = self.run_test262_module(
            """
let outcome;
let order = [];
Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value.then(value => outcome = value);
Promise.resolve().then(() => order.push("promise"));
function poll() {
    if (order[0] !== "promise") throw new Error("Timer ran before the promise checkpoint");
    if (outcome === "timed-out") {
        print("Test262:AsyncTestComplete");
        return;
    }
    setTimeout(poll, 0);
}
setTimeout(poll, 0);
$262.gc();
""",
            flags="async, raw",
        )
        self.assertEqual(result["result"], "passed", result)

    def test_runner_agent_rejection_stops_an_unrelated_infinite_wait(self):
        for reaction in [
            "Promise.resolve()",
            "Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value",
        ]:
            with self.subTest(reaction=reaction):
                result = self.run_test262_module(
                    f"""
$262.agent.start(`
    globalThis.pending = Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0).value;
    {reaction}.then(() => {{ throw new Error("agent rejection sentinel"); }});
`);
while ($262.agent.getReport() === null) {{
    $262.agent.sleep(1);
}}
""",
                    flags="raw",
                )
                self.assertEqual(result["result"], "failed", result)
                self.assertIn("agent rejection sentinel", json.dumps(result))

    def test_runner_agent_rejection_handled_at_checkpoint_can_continue(self):
        result = self.run_test262_module(
            """
$262.agent.start(`
    globalThis.pending = Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0).value;
    const rejected = Promise.reject(new Error("handled agent rejection"));
    Promise.resolve().then(() => rejected.catch(() => {}));
    Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 1).value.then(() => {
        $262.agent.report("agent continued");
        $262.agent.leaving();
    });
`);
let report;
while ((report = $262.agent.getReport()) === null) {
    $262.agent.sleep(1);
}
if (report !== "agent continued") throw new Error("Unexpected agent report");
""",
            flags="raw",
        )
        self.assertEqual(result["result"], "passed", result)

    def test_runner_timer_exception_fails(self):
        result = self.run_test262_module(
            """
setTimeout(() => { throw new Error("timer exception sentinel"); }, 0);
setTimeout(() => print("Test262:AsyncTestComplete"), 0);
""",
            flags="async, raw",
        )
        self.assertNotEqual(result["result"], "passed", result)
        self.assertIn("timer exception sentinel", json.dumps(result))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--js", required=True, type=Path)
    parser.add_argument("--test262-runner", type=Path)
    arguments, remaining = parser.parse_known_args()
    ModuleHostTests.js = arguments.js.resolve()
    ModuleHostTests.test262_runner = arguments.test262_runner.resolve() if arguments.test262_runner else None
    unittest.main(argv=[sys.argv[0], *remaining])
