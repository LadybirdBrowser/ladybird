"""State-flag invariants — what has "focusable", "editable", "disabled", etc.

Key problem to guard against regressing: marking paragraphs/headings/list items focusable breaks Orca's sentence
extension in Say All — because is_text_block_element returns false for focusable objects."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import find_first_by_role  # noqa: E402


def _has_state(obj, state_type):
    states = obj.get_state_set()
    return states.contains(state_type)


class FocusableStateTests(AccessibilityBridgeTestCase):
    FIXTURE = "states.html"

    def test_button_is_focusable(self):
        """Buttons are focusable."""
        btns = find_all_by_role(self.doc, "button")
        normal = next((b for b in btns if b.get_name() == "Normal button"), None)
        self.assertIsNotNone(normal)
        self.assertTrue(_has_state(normal, Atspi.StateType.FOCUSABLE))

    def test_link_is_focusable(self):
        """Links are focusable."""
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link)
        self.assertTrue(_has_state(link, Atspi.StateType.FOCUSABLE))

    def test_text_input_is_focusable(self):
        """Text inputs are focusable.

        Note: we don't currently set STATE_EDITABLE — Orca uses role to detect editable fields."""
        entry = None
        for e in find_all_by_role(self.doc, "entry"):
            if e.get_name() == "Text field":
                entry = e
                break
        if entry is None:
            for e in find_all_by_role(self.doc, "text"):
                if e.get_name() == "Text field":
                    entry = e
                    break
        self.assertIsNotNone(entry, "could not find the plain text field")
        self.assertTrue(_has_state(entry, Atspi.StateType.FOCUSABLE))

    @unittest.expectedFailure
    def test_contenteditable_div_is_focusable(self):
        """Editable elements are focusable even when role isn't in the explicit list.

        KNOWN GAP: "<div contenteditable>" is exposed with role "section" (mapped from generic+name) and does *not* get
        STATE_FOCUSABLE. That's either because LibWeb doesn't populate "is_editable=true" for it — or because the Qt
        bridge doesn't honor "data->is_editable" for the section-mapped path. So this test is, for now, intentionally
        left in place here — but with an expected-failure marker, so we notice when the gap is closed."""
        editable = None
        from harness import walk

        def visit(obj, _depth):
            nonlocal editable
            if editable is not None:
                return
            try:
                if obj.get_name() == "Editable div":
                    editable = obj
            except Exception:
                pass

        walk(self.doc, visit)
        self.assertIsNotNone(editable, "could not find the contenteditable div")
        self.assertTrue(_has_state(editable, Atspi.StateType.FOCUSABLE))

    def test_paragraph_is_not_focusable(self):
        """Paragraphs are *not* focusable. Required for Orca's is_text_block_element."""
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0)
        for p in ps:
            self.assertFalse(
                _has_state(p, Atspi.StateType.FOCUSABLE), f"paragraph {p.get_name()!r} must *not* be focusable"
            )

    def test_heading_is_not_focusable(self):
        """Headings are *not* focusable."""
        hs = find_all_by_role(self.doc, "heading")
        # h1 from the fixture
        # Don't fail if fixture has no heading — we just check what *is* there.
        for h in hs:
            self.assertFalse(
                _has_state(h, Atspi.StateType.FOCUSABLE), f"heading {h.get_name()!r} must *not* be focusable"
            )

    def test_listitem_is_not_focusable(self):
        """List items are *not* focusable."""
        lis = find_all_by_role(self.doc, "list item")
        for li in lis:
            self.assertFalse(_has_state(li, Atspi.StateType.FOCUSABLE), "listitem must *not* be focusable")

    def test_list_is_not_focusable(self):
        """Lists are *not* focusable."""
        lsts = find_all_by_role(self.doc, "list")
        for lst in lsts:
            self.assertFalse(_has_state(lst, Atspi.StateType.FOCUSABLE), "list must *not* be focusable")


class DisabledStateTests(AccessibilityBridgeTestCase):
    FIXTURE = "states.html"

    def test_disabled_button_has_not_sensitive_state(self):
        """<button disabled> reports the disabled state."""
        btns = find_all_by_role(self.doc, "button")
        disabled = next((b for b in btns if b.get_name() == "Disabled button"), None)
        self.assertIsNotNone(disabled)
        # Atspi disabled elements are reported as *not* sensitive (no STATE_SENSITIVE/STATE_ENABLED).
        is_disabled = not _has_state(disabled, Atspi.StateType.SENSITIVE) or not _has_state(
            disabled, Atspi.StateType.ENABLED
        )
        self.assertTrue(is_disabled, "disabled button must *not* have SENSITIVE+ENABLED states")


if __name__ == "__main__":
    unittest.main()
