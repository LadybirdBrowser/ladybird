"""Launch Ladybird on a fixture page and connect to its AT-SPI2 accessible."""

from __future__ import annotations

import os
import pathlib
import subprocess
import tempfile
import time

from contextlib import contextmanager
from typing import Callable
from typing import Optional

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
DEFAULT_BINARY = REPO_ROOT / "Build" / "release" / "bin" / "Ladybird"
FIXTURE_DIR = pathlib.Path(__file__).resolve().parents[1] / "input"


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
    """A running Ladybird process wrapped with AT-SPI2 discovery helpers.

    One context per test; a new Ladybird process is spawned per context — so state from a previous test cannot leak. Use
    via the "ladybird" pytest fixture or the ladybird_for_fixture context manager."""

    def __init__(self, url: str, binary: Optional[pathlib.Path] = None):
        self._url = url
        if binary is not None:
            self._binary = pathlib.Path(binary)
        elif os.environ.get("LADYBIRD_BINARY"):
            self._binary = pathlib.Path(os.environ["LADYBIRD_BINARY"])
        else:
            self._binary = DEFAULT_BINARY
        self._process: Optional[subprocess.Popen] = None
        self._app: Optional[Atspi.Accessible] = None
        self._stderr_file: Optional[tempfile.NamedTemporaryFile] = None

    def __enter__(self) -> "LadybirdContext":
        self.start()
        return self

    def __exit__(self, *exc_info) -> None:
        self.stop()

    def start(self) -> None:
        """Spawns Ladybird and waits for its AT-SPI2 app to register."""
        if not self._binary.exists():
            raise FileNotFoundError(
                f"Ladybird binary not found at {self._binary}. Build it with 'ninja -C Build/release ladybird'."
            )

        env = os.environ.copy()
        # Force a Qt platform we can drive headlessly. xcb works under Xvfb.
        env.setdefault("QT_QPA_PLATFORM", "xcb")
        # Silence Qt's a11y logging noise — so failures are readable.
        env.setdefault("QT_LOGGING_RULES", "qt.accessibility.atspi=false")
        # By default, Qt only activates its AT-SPI2 bridge when something signals that AT is "enabled" — usually a
        # desktop-environment toolkit setting (e.g. GNOME's org.gnome.desktop.interface.toolkit-accessibility, which
        # gets reflected onto the org.a11y.Status D-Bus property). On a headless CI runner there's no DE backend setting
        # that flag — so even with at-spi-bus-launcher and at-spi2-registryd both alive, Qt declines to attach. This
        # environment variable tells Qt to skip the IsEnabled check, and bring the bridge up unconditionally.
        env.setdefault("QT_LINUX_ACCESSIBILITY_ALWAYS_ON", "1")

        # Send stderr to a regular file rather than subprocess.PIPE: Ladybird debug builds (ENABLE_ALL_THE_DEBUG_MACROS)
        # produce huge stderr volumes (PROMISE_DEBUG, etc.). A pipe's kernel buffer is ~64 KB; once it fills, every
        # dbgln in WebContent blocks on write — which can wedge the engine before AT-SPI2 registration completes. A
        # regular file never backpressures the writer, and we still get the bytes for diagnostics on failure.
        self._stderr_file = tempfile.NamedTemporaryFile(prefix="ladybird-a11y-stderr-", suffix=".log", delete=False)

        # --force-cpu-painting: skip the GPU/Vulkan path. Headless CI runners have no Vulkan ICD, and per
        # Services/WebContent/main.cpp the CPU backend is also what other test harnesses use ("the GPU backend is not
        # deterministic"). The Vulkan probe failure itself is non-fatal — we just don't want it cluttering diagnostics.
        self._process = subprocess.Popen(
            [str(self._binary), "--force-cpu-painting", self._url],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=self._stderr_file,
        )

        # Start a fresh Atspi connection for this context. Atspi.init() is idempotent — subsequent tests share the
        # process-global connection.
        Atspi.init()

        def _app_appeared() -> bool:
            app = self._find_app_on_desktop()
            if app is None:
                return False
            # Wait for the document tree to arrive, not just the app object. The document-web accessible can show up on
            # the bus before Ladybird has populated it with children — and, when tests run back-to-back, that racy empty
            # state can otherwise leak into tests as flaky "expected X in the tree" failures.
            try:
                doc = self.find_document_web(app)
            except Exception:
                return False
            if doc is None:
                return False
            try:
                if doc.get_child_count() == 0:
                    return False
            except Exception:
                return False
            self._app = app
            return True

        startup_timeout = float(os.environ.get("LADYBIRD_AT_SPI2_STARTUP_TIMEOUT", "60"))
        if not wait_for(_app_appeared, timeout=startup_timeout, description="Ladybird AT-SPI2 app to appear"):
            # Snapshot diagnostics *before* stop() tears the process and stderr file down. The point of this branch is
            # to make the next CI run's failure say *why* the app didn't appear: did Ladybird die? Is something else
            # registered on the desktop? Did Ladybird register under a different name?
            expected_pid = self._process.pid if self._process is not None else None
            process_alive = self._process is not None and self._process.poll() is None
            process_returncode = self._process.returncode if self._process is not None else None
            desktop_children: list[str] = []
            try:
                desktop = Atspi.get_desktop(0)
                for i in range(desktop.get_child_count()):
                    child = desktop.get_child_at_index(i)
                    if child is None:
                        desktop_children.append(f"[{i}] <None>")
                        continue
                    try:
                        child_name = child.get_name() or "<unnamed>"
                    except Exception as e:
                        child_name = f"<get_name failed: {e}>"
                    try:
                        child_pid = child.get_process_id()
                    except Exception:
                        child_pid = None
                    desktop_children.append(f"[{i}] name={child_name!r} pid={child_pid}")
            except Exception as e:
                desktop_children.append(f"<desktop enumeration failed: {e}>")

            stderr = ""
            if self._stderr_file is not None:
                try:
                    with open(self._stderr_file.name, "rb") as f:
                        stderr = f.read().decode(errors="replace")
                except Exception:
                    pass
            self.stop()
            raise RuntimeError(
                f"Ladybird did not register on AT-SPI2 within {startup_timeout:g}s.\n"
                f"URL: {self._url}\nBinary: {self._binary}\n"
                f"Expected PID: {expected_pid}, alive: {process_alive}, returncode: {process_returncode}\n"
                f"AT-SPI2 desktop children ({len(desktop_children)}):\n  "
                + "\n  ".join(desktop_children)
                + f"\nStderr: {stderr[-2000:]}"
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

    def _find_app_on_desktop(self) -> Optional[Atspi.Accessible]:
        expected_pid = self._process.pid if self._process is not None else None
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            child = desktop.get_child_at_index(i)
            if child is None:
                continue
            try:
                name = child.get_name() or ""
            except Exception:
                continue
            if "ladybird" not in name.lower():
                continue
            # A previous test case's Ladybird can linger on the desktop while its accessible children are already gone.
            # To deal with that, pin the match to our own subprocess PID — so we don't latch onto a dying predecessor.
            if expected_pid is not None:
                try:
                    if child.get_process_id() != expected_pid:
                        continue
                except Exception:
                    continue
            return child
        return None

    # Accessors. All of these are best-effort and may return None for a still-loading page.

    @property
    def app(self) -> Atspi.Accessible:
        if self._app is None:
            raise RuntimeError("Ladybird context not started. Call start() or use as context manager.")
        return self._app

    @staticmethod
    def find_document_web(root: Atspi.Accessible) -> Optional[Atspi.Accessible]:
        return _find_by_role(root, "document web")

    @property
    def document(self) -> Atspi.Accessible:
        doc = self.find_document_web(self.app)
        if doc is None:
            raise RuntimeError("document web accessible not present in Ladybird tree")
        return doc


def _find_by_role(root: Atspi.Accessible, role_name: str, depth: int = 0) -> Optional[Atspi.Accessible]:
    if root is None or depth > 30:
        return None
    try:
        if root.get_role_name() == role_name:
            return root
    except Exception:
        pass
    for i in range(root.get_child_count()):
        child = root.get_child_at_index(i)
        if child is None:
            continue
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
