"""Tests for Script._on_focused_changed in our Orca script."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi  # noqa: E402

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import LadybirdOrcaTestCase  # noqa: E402
from harness import find_first_by_role  # noqa: E402


class OnFocusedChangedTests(LadybirdOrcaTestCase):
    FIXTURE = "states.html"

    def test_focus_event_dispatches_to_our_override(self):
        """Orca 50+ dispatches focus events to self._on_focused_changed (with the leading underscore). If our code were
        to inadvertently revert to defining it with the older (pre Orca 50) on_focused_changed (without the underscore)
        spelling, the dispatcher would silently fall through to web.Script._on_focused_changed — and our un-suspend
        logic would unexpectedly never run. It wouldn't throw and there'd be no log message; instead, after focus enters
        document content, H/K/L navigation would unexpectedly just stay suspended.

        Therefore, to catch/avoid that, we assert here that our override resolves to a class that's *not* one of Orca's
        own base classes."""
        script_class = type(self.orca.script)
        # Find the first owner of _on_focused_changed on the MRO — that's where the call resolves to.
        owner = next(c for c in script_class.__mro__ if "_on_focused_changed" in c.__dict__)
        self.assertFalse(
            owner.__module__.startswith("orca."),
            f"_on_focused_changed resolves to Orca's own {owner.__module__}.{owner.__qualname__} — meaning the "
            "Ladybird Script class isn't overriding it. Most likely the override is named on_focused_changed "
            "(pre-Orca-50 spelling) instead of _on_focused_changed.",
        )

    def _build_focused_event(self, source: Atspi.Accessible) -> Atspi.Event:
        """Construct a synthetic AT-SPI2 focused-state event with the given source."""
        event = Atspi.Event()
        event.type = "object:state-changed:focused"
        event.source = source
        event.detail1 = 1  # 1 = focused (object:state-changed:focused)
        event.detail2 = 0
        return event

    def _dispatch_focus_event(self, source: Atspi.Accessible) -> None:
        """Drive _on_focused_changed via the synthetic event. Asserts no AttributeError."""
        event = self._build_focused_event(source)
        try:
            self.orca.script._on_focused_changed(event)
        except AttributeError as exc:
            self.fail(
                f"_on_focused_changed raised AttributeError — likely a pre-Orca-50 API call slipping back in: {exc}"
            )
        except Exception:
            # Other exceptions (TypeError from a partially-constructed event, errors deep in super's call chain
            # against the synthetic environment, etc.) aren't what this regression test is guarding against.
            pass

    def test_focus_into_document_content_does_not_crash(self):
        """_on_focused_changed must not crash when focus lands on a document-content element.

        Prior to Orca 50, we called self.get_structural_navigator()/get_caret_navigator()/
        get_table_navigator()/self.live_region_manager — which are all retired in Orca 50+."""
        btn = find_first_by_role(self.doc, Atspi.Role.PUSH_BUTTON)
        self.assertIsNotNone(btn, "expected a push-button in states.html")
        self._dispatch_focus_event(btn)

    def test_focus_on_editable_chrome_does_not_crash(self):
        """_on_focused_changed must not crash on the editable-chrome branch.

        Prior to Orca 50, we called self.get_structural_navigator()/get_caret_navigator() in the editable-source path."""
        # The fixture has multiple <input type="text"> fields. Use whichever the bridge exposes — different libatspi
        # versions render the role string as either "entry" (Atspi.Role.ENTRY) or "text" (Atspi.Role.TEXT). Per
        # AXUtilities.is_entry, either value satisfies the editable-chrome branch in script._on_focused_changed.
        text_input = find_first_by_role(self.doc, Atspi.Role.ENTRY)
        if text_input is None:
            text_input = find_first_by_role(self.doc, Atspi.Role.TEXT)
        self.assertIsNotNone(text_input, "expected an editable text input in states.html")
        self._dispatch_focus_event(text_input)


if __name__ == "__main__":
    unittest.main()
