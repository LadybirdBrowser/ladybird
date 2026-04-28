"""Layer-2 Orca harness: load Orca, instantiate our Ladybird script against a live Ladybird AT-SPI2 accessible, and
capture every speech.speak call so tests can assert on what Orca *would* have said.

This is how we catch regressions at the screen-reader-user layer — above the bridge, inside Orca's own speech-generation
code paths. Bugs like the I-key regression (list item reads only its first character) live here: the bridge output is
technically correct but the end-to-end speech is wrong — and only a test that drives Orca's _generate_list_item →
speech.speak chain can see that.

The Orca script under test is loaded directly from the repo (UI/Qt/OrcaScripts/Ladybird/), not from the installed copy
at ~/.local/share/orca/orca-scripts/Ladybird/. That way, what we test is what we check in."""

from __future__ import annotations

import importlib
import importlib.util
import pathlib
import site
import sys

from typing import Any
from typing import List
from typing import Optional

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[4]
LADYBIRD_ORCA_SCRIPTS_SRC = REPO_ROOT / "UI" / "Qt" / "OrcaScripts"


def _find_orca_site_packages() -> Optional[pathlib.Path]:
    """Locate Orca's install directory in Python's site-packages search path."""
    for path in site.getsitepackages() + [site.getusersitepackages()]:
        candidate = pathlib.Path(path) / "orca"
        if candidate.is_dir() and (candidate / "script_manager.py").exists():
            return candidate.parent
    # Fall back to probing common distro paths (e.g., Fedora, Arch).
    for probe in [
        "/usr/lib/python3.14/site-packages",
        "/usr/lib/python3.13/site-packages",
        "/usr/lib/python3.12/site-packages",
    ]:
        p = pathlib.Path(probe) / "orca"
        if p.is_dir() and (p / "script_manager.py").exists():
            return p.parent
    return None


_orca_initialized = False


class OrcaNotInstalled(Exception):
    """Raised when Orca is not importable. Layer 2 test classes should catch this in setUp and skip."""


def _initialize_orca_once() -> None:
    """Set up sys.path + activate Orca's settings manager. Call before importing any orca module.

    Must be done exactly once per process. Repeating it risks double-initializing singletons inside Orca."""
    global _orca_initialized
    if _orca_initialized:
        return

    orca_site = _find_orca_site_packages()
    if orca_site is None:
        raise OrcaNotInstalled("could not locate Orca's site-packages directory. Install Orca to run Layer 2 tests.")
    if str(orca_site) not in sys.path:
        sys.path.insert(0, str(orca_site))

    # Orca's internal modules have circular imports that only resolve cleanly when script_manager is imported first.
    # Respect that.
    try:
        importlib.import_module("orca.script_manager")
    except ImportError as e:
        raise OrcaNotInstalled(f"failed to import orca: {e}") from e

    _orca_initialized = True


def _load_ladybird_script_class():
    """Import our Ladybird Orca script directly from the repo source.

    We load via importlib.util rather than "import" so the script tree in UI/Qt/OrcaScripts/ doesn't need to be a
    Python package (adding __init__.py to that directory would bloat the Qt resource bundle, and the layout has to
    match Orca's runtime expectation of orca-scripts/Ladybird/)."""
    _initialize_orca_once()

    # First load the script_utilities submodule, then "script" — which imports it via a relative
    # "from .script_utilities import Utilities". Register both under a package name so the relative import resolves.
    package_name = "ladybird_orca_script_under_test"
    ladybird_dir = LADYBIRD_ORCA_SCRIPTS_SRC / "Ladybird"

    # Pre-create the package by registering a synthetic module whose __path__ is the directory — then relative imports
    # inside it work.
    if package_name not in sys.modules:
        pkg_spec = importlib.util.spec_from_file_location(
            package_name,
            ladybird_dir / "__init__.py",
            submodule_search_locations=[str(ladybird_dir)],
        )
        pkg = importlib.util.module_from_spec(pkg_spec)
        sys.modules[package_name] = pkg
        pkg_spec.loader.exec_module(pkg)

    # Now load script.py as a submodule. script.py does "from .script_utilities import Utilities" — which triggers
    # loading script_utilities.py from the package directory.
    script_spec = importlib.util.spec_from_file_location(
        f"{package_name}.script",
        ladybird_dir / "script.py",
    )
    script_mod = importlib.util.module_from_spec(script_spec)
    sys.modules[f"{package_name}.script"] = script_mod
    script_spec.loader.exec_module(script_mod)
    return script_mod.Script


class OrcaSession:
    """Wraps the setup/teardown of an Orca-in-process test session.

    Each test method should get a fresh OrcaSession - so captured speech doesn't leak between tests. Speech is
    monkey-patched on start() and restored on stop()."""

    def __init__(self, app: Atspi.Accessible):
        self._app = app
        self._script = None
        self._captured: List[Any] = []
        self._orig_speak = None
        self._speech_module = None

    def __enter__(self) -> "OrcaSession":
        self.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stop()

    def start(self) -> None:
        script_cls = _load_ladybird_script_class()
        self._script = script_cls(self._app)

        # Orca 50's web.Script.present_object delegates speech generation to presentation_manager.speak_contents →
        # speech_presenter.speak_contents — which early-returns when script_manager has no active script registered.
        # Real Orca registers the active script via focus events — but our isolated test never fires one. So, we
        # register it directly.
        from orca import script_manager  # noqa: E402

        script_manager.get_manager().set_active_script(self._script, "Layer 2 test harness")

        # Intercept speech. Orca 50's speech.speak signature is (content, acss=None); 49 also accepted interrupt
        # positionally, so we keep that name for back-compat with any code path that still passes it.
        from orca import speech  # noqa: E402

        self._speech_module = speech
        self._orig_speak = speech.speak

        def _capture(content=None, acss=None, interrupt=True):
            self._captured.append(content)

        speech.speak = _capture

    def stop(self) -> None:
        if self._speech_module is not None and self._orig_speak is not None:
            self._speech_module.speak = self._orig_speak
            self._orig_speak = None
            self._speech_module = None
        # Deregister the active script — without this, a subsequent test's OrcaSession.start() would deactivate our
        # previous instance's structural_navigator state via the set_active_script side effects.
        try:
            from orca import script_manager  # noqa: E402

            script_manager.get_manager().set_active_script(None, "Layer 2 test teardown")
        except Exception:
            pass
        self._script = None

    @property
    def script(self):
        if self._script is None:
            raise RuntimeError("OrcaSession not started")
        return self._script

    def clear_captured(self) -> None:
        self._captured.clear()

    def captured_strings(self) -> List[str]:
        """Flatten every captured speech.speak payload into a list of strings.

        Orca's utterances are lists that mix str, ACSS (voice-settings dicts), and Pause objects. We just pull out the
        strings."""
        out: List[str] = []
        for payload in self._captured:
            if payload is None:
                continue
            if isinstance(payload, str):
                out.append(payload)
                continue
            if isinstance(payload, list):
                for item in payload:
                    if isinstance(item, str):
                        out.append(item)
                    elif isinstance(item, list):
                        for inner in item:
                            if isinstance(inner, str):
                                out.append(inner)
        return [s for s in out if s]

    def captured_text(self) -> str:
        """All captured speech joined into a single string, for substring assertions."""
        return " | ".join(self.captured_strings())


def utilities(script_or_session):
    """Shortcut: return the Utilities object for either a Script or an OrcaSession."""
    if hasattr(script_or_session, "script"):
        return script_or_session.script.utilities
    return script_or_session.utilities
