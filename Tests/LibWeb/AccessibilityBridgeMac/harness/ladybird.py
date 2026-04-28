"""Launch Ladybird on a fixture page and connect to it via NSAccessibility/AXUIElement."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time

from contextlib import contextmanager
from typing import Callable
from typing import Optional

from ApplicationServices import AXIsProcessTrusted
from ApplicationServices import AXUIElementCopyAttributeValue
from ApplicationServices import AXUIElementCreateApplication
from ApplicationServices import kAXChildrenAttribute
from ApplicationServices import kAXFocusedWindowAttribute
from ApplicationServices import kAXMainWindowAttribute
from ApplicationServices import kAXRoleAttribute

REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]

DEFAULT_BINARY = REPO_ROOT / "Build" / "release" / "bin" / "Ladybird.app" / "Contents" / "MacOS" / "Ladybird"

FIXTURE_DIR = pathlib.Path(__file__).resolve().parents[2] / "AccessibilityBridge" / "input"


def _ax_attr(elem, key):
    """Read one AX attribute. Returns None on any error (element gone, attribute unsupported, etc.)."""
    if elem is None:
        return None
    try:
        err, value = AXUIElementCopyAttributeValue(elem, key, None)
    except Exception:
        return None
    return None if err != 0 else value


def wait_for(
    predicate: Callable[[], bool],
    *,
    timeout: float = 15.0,
    interval: float = 0.1,
    description: str = "condition",
) -> bool:
    """Polls predicate() until it returns truthy or timeout expires.

    Returns whatever the final predicate call returned (truthy = success). The test file is responsible for asserting on
    the return value when it needs to fail the test on timeout; this helper never raises on timeout by itself — so
    callers can build their own error messages."""

    deadline = time.monotonic() + timeout
    result = False
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(interval)
    return result


class LadybirdContext:
    """A running Ladybird process wrapped with NSAccessibility/AXUIElement discovery helpers.

    One context per test class; a new Ladybird process is spawned per context — so state from a previous test cannot
    leak. Use via AccessibilityBridgeMacTestCase or the ladybird_for_fixture context manager."""

    def __init__(self, url: str, binary: Optional[pathlib.Path] = None):
        self._url = url
        if binary is not None:
            self._binary = pathlib.Path(binary)
        elif os.environ.get("LADYBIRD_BINARY"):
            self._binary = pathlib.Path(os.environ["LADYBIRD_BINARY"])
        else:
            self._binary = DEFAULT_BINARY
        self._process: Optional[subprocess.Popen] = None
        self._app = None
        self._web = None
        self._stderr_file: Optional[tempfile.NamedTemporaryFile] = None

    def __enter__(self) -> "LadybirdContext":
        self.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def start(self) -> None:
        """Spawns Ladybird and waits for its AXWebArea to become populated."""
        if not self._binary.exists():
            raise FileNotFoundError(
                f"Ladybird binary not found at {self._binary}. Build it with 'ninja -C Build/release ladybird'."
            )

        # macOS gates AXUIElement reads behind the Accessibility permission. If the test runner doesn't have it, every
        # AX call silently returns kAXErrorAPIDisabled and the harness hangs indefinitely — so fail loud; fail early.
        if not AXIsProcessTrusted():
            raise RuntimeError(
                "The test runner is not trusted for Accessibility (kAXErrorAPIDisabled). "
                "Grant the calling shell/IDE Accessibility permission in System Settings → "
                "Privacy & Security → Accessibility, then re-run."
            )

        env = os.environ.copy()

        # Send stderr to a regular file rather than subprocess.PIPE: Ladybird debug builds (ENABLE_ALL_THE_DEBUG_MACROS)
        # produce huge stderr volumes (PROMISE_DEBUG, etc.). A pipe's kernel buffer is ~64 KB; once it fills, every
        # dbgln in WebContent blocks on write — which can wedge the engine before AX registration completes. A regular
        # file never backpressures the writer, and we still get the bytes for diagnostics on failure.
        self._stderr_file = tempfile.NamedTemporaryFile(prefix="ladybird-ax-stderr-", suffix=".log", delete=False)

        # --force-cpu-painting: skip the GPU/Metal path. Per Services/WebContent/main.cpp the CPU backend is what other
        # test harnesses use ("the GPU backend is not deterministic"), and these AX tests don't depend on rendering at
        # all — they inspect the NSAccessibility surface, which is independent of which painter ran.
        self._process = subprocess.Popen(
            [str(self._binary), "--force-cpu-painting", self._url],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=self._stderr_file,
        )

        self._app = AXUIElementCreateApplication(self._process.pid)

        def _web_appeared() -> bool:
            web = self._find_web_area()
            if web is None:
                return False
            # Wait for the AXWebArea's children to populate — not just the AXWebArea itself. The web area can show up on
            # the AX surface before Ladybird has populated it with children — and, when tests run back-to-back, that
            # racy empty state can leak into tests as flaky "expected X in the tree" failures.
            children = _ax_attr(web, kAXChildrenAttribute)
            if not children:
                return False
            self._web = web
            return True

        startup_timeout = float(os.environ.get("LADYBIRD_AX_STARTUP_TIMEOUT", "60"))
        if not wait_for(_web_appeared, timeout=startup_timeout, description="Ladybird AXWebArea to populate"):
            # Read the stderr file *before* stop() — stop() unlinks it.
            stderr = ""
            if self._stderr_file is not None:
                try:
                    with open(self._stderr_file.name, "rb") as f:
                        stderr = f.read().decode(errors="replace")
                except Exception:
                    pass
            self.stop()
            raise RuntimeError(
                f"Ladybird did not register an AXWebArea on NSAccessibility within {startup_timeout:g}s.\n"
                f"URL: {self._url}\nBinary: {self._binary}\nStderr: {stderr[-2000:]}"
            )

    def stop(self) -> None:
        if self._process is None:
            self._cleanup_stderr_file()
            return
        try:
            self._process.terminate()
            try:
                self._process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._process.kill()
                self._process.wait(timeout=5)
        finally:
            self._process = None
            self._app = None
            self._web = None
            self._cleanup_stderr_file()

    def _cleanup_stderr_file(self) -> None:
        if self._stderr_file is None:
            return
        try:
            self._stderr_file.close()
        except Exception:
            pass
        try:
            os.unlink(self._stderr_file.name)
        except Exception:
            pass
        self._stderr_file = None

    def _find_web_area(self):
        """Walk from the application's focused/main window down to the first AXWebArea descendant."""
        window = _ax_attr(self._app, kAXFocusedWindowAttribute) or _ax_attr(self._app, kAXMainWindowAttribute)
        if window is None:
            return None
        return _find_by_role(window, "AXWebArea")

    @property
    def app(self):
        """The application AXUIElement. Raises if start() hasn't completed."""
        if self._app is None:
            raise RuntimeError("Ladybird context not started. Call start() or use as context manager.")
        return self._app

    @property
    def web(self):
        """The AXWebArea (the document root in NSAccessibility terms). Raises if not yet present."""
        if self._web is None:
            raise RuntimeError("AXWebArea not present in Ladybird tree (start() may not have completed)")
        return self._web


def _find_by_role(root, role_name: str, depth: int = 0):
    """Pre-order DFS for the first descendant with matching AXRole. Depth-capped to defeat pathological trees."""
    if root is None or depth > 30:
        return None
    if _ax_attr(root, kAXRoleAttribute) == role_name:
        return root
    for child in _ax_attr(root, kAXChildrenAttribute) or []:
        found = _find_by_role(child, role_name, depth + 1)
        if found is not None:
            return found
    return None


@contextmanager
def ladybird_for_fixture(fixture_name: str, binary: Optional[pathlib.Path] = None):
    """Context manager spawning Ladybird on input/<fixture_name>."""
    fixture_path = (FIXTURE_DIR / fixture_name).resolve()
    if not fixture_path.exists():
        raise FileNotFoundError(f"fixture HTML not found: {fixture_path}")
    url = fixture_path.as_uri()
    ctx = LadybirdContext(url, binary=binary)
    try:
        ctx.start()
        yield ctx
    finally:
        ctx.stop()
