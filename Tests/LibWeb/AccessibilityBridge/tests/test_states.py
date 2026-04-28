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
from harness import get_attributes_dict  # noqa: E402
from harness import supports_interface  # noqa: E402
from harness import wait_for  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
from harness import walk  # noqa: E402


def _has_state(obj, state_type):
    states = obj.get_state_set()
    return states.contains(state_type)


class FocusableStateTests(AccessibilityBridgeTestCase):
    FIXTURE = "states.html"

    def test_button_is_focusable(self):
        """Buttons are focusable."""
        normal = wait_for_descendant_by_role(self.doc, Atspi.Role.PUSH_BUTTON, name="Normal button")
        self.assertIsNotNone(normal, "expected 'Normal button' in the tree")
        self.assertTrue(_has_state(normal, Atspi.StateType.FOCUSABLE))

    def test_link_is_focusable(self):
        """Links are focusable."""
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link)
        self.assertTrue(_has_state(link, Atspi.StateType.FOCUSABLE))

    def test_text_input_is_focusable(self):
        """Text inputs are focusable. EditableStateTests covers their EDITABLE state."""
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

    def test_contenteditable_div_is_focusable(self):
        """Editable elements are focusable even when role isn't in the explicit list.

        A "<div contenteditable>" is an editing host, so it is exposed with is_editable set, which the bridge maps to
        STATE_FOCUSABLE even though the role is section."""
        editable = None

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
        self.assertGreater(len(hs), 0, "expected the fixture heading in the tree")
        for h in hs:
            self.assertFalse(
                _has_state(h, Atspi.StateType.FOCUSABLE), f"heading {h.get_name()!r} must *not* be focusable"
            )

    def test_listitem_is_not_focusable(self):
        """List items are *not* focusable."""
        lis = find_all_by_role(self.doc, "list item")
        self.assertGreater(len(lis), 0, "expected the fixture's list item in the tree")
        for li in lis:
            self.assertFalse(_has_state(li, Atspi.StateType.FOCUSABLE), "listitem must *not* be focusable")

    def test_list_is_not_focusable(self):
        """Lists are *not* focusable."""
        lsts = find_all_by_role(self.doc, "list")
        self.assertGreater(len(lsts), 0, "expected the fixture's list in the tree")
        for lst in lsts:
            self.assertFalse(_has_state(lst, Atspi.StateType.FOCUSABLE), "list must *not* be focusable")


class DisabledStateTests(AccessibilityBridgeTestCase):
    FIXTURE = "states.html"

    def test_disabled_button_has_not_sensitive_state(self):
        """<button disabled> reports the disabled state."""
        disabled = wait_for_descendant_by_role(self.doc, Atspi.Role.PUSH_BUTTON, name="Disabled button")
        self.assertIsNotNone(disabled, "expected 'Disabled button' in the tree")
        # Atspi maps a disabled element to *both* STATE_SENSITIVE and STATE_ENABLED cleared, so require both absent.
        self.assertFalse(_has_state(disabled, Atspi.StateType.SENSITIVE), "disabled button must *not* be SENSITIVE")
        self.assertFalse(_has_state(disabled, Atspi.StateType.ENABLED), "disabled button must *not* be ENABLED")


class EditableStateTests(AccessibilityBridgeTestCase):
    """The text-editing bits: EDITABLE, READ_ONLY, MULTI_LINE and MULTISELECTABLE.

    Qt's AT-SPI adaptor maps them one to one from state() (spiStatesFromQState), and Orca reads EDITABLE to treat a text
    object as an entry: focus mode, typing echo. Without it, a textbox whose role the bridge reports as "text" is just
    static text to Orca."""

    FIXTURE = "states.html"

    def _named(self, name):
        found = None

        def visit(obj, _depth):
            nonlocal found
            if found is None and obj.get_name() == name:
                found = obj

        def look():
            walk(self.doc, visit)
            return found is not None

        self.assertTrue(wait_for(look, description=f"{name!r} on the bus"), f"could not find {name!r}")
        return found

    def test_text_input_is_editable(self):
        field = self._named("Text field")
        self.assertTrue(_has_state(field, Atspi.StateType.EDITABLE), "a text input must be EDITABLE")
        self.assertFalse(_has_state(field, Atspi.StateType.READ_ONLY))
        self.assertFalse(_has_state(field, Atspi.StateType.MULTI_LINE), "a text input is single-line")

    def test_readonly_input_is_read_only_not_editable(self):
        field = self._named("Readonly text")
        self.assertTrue(_has_state(field, Atspi.StateType.READ_ONLY), "a readonly input must be READ_ONLY")
        self.assertFalse(_has_state(field, Atspi.StateType.EDITABLE), "a readonly input must not be EDITABLE")

    def test_textarea_is_editable_and_multi_line(self):
        area = self._named("Text area")
        self.assertTrue(_has_state(area, Atspi.StateType.EDITABLE))
        self.assertTrue(_has_state(area, Atspi.StateType.MULTI_LINE), "a textarea must be MULTI_LINE")

    def test_contenteditable_is_editable_and_multi_line(self):
        editable = self._named("Editable div")
        self.assertTrue(_has_state(editable, Atspi.StateType.EDITABLE), "an editing host must be EDITABLE")
        self.assertTrue(_has_state(editable, Atspi.StateType.MULTI_LINE))

    def test_multiselectable_tree(self):
        tree = self._named("Multi tree")
        self.assertTrue(
            _has_state(tree, Atspi.StateType.MULTISELECTABLE), "aria-multiselectable must be MULTISELECTABLE"
        )
        for lst in find_all_by_role(self.doc, "list"):
            self.assertFalse(
                _has_state(lst, Atspi.StateType.MULTISELECTABLE), "a plain list must not be MULTISELECTABLE"
            )

    def test_paragraph_is_not_editable(self):
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0)
        for p in ps:
            self.assertFalse(
                _has_state(p, Atspi.StateType.EDITABLE), f"paragraph {p.get_name()!r} must not be EDITABLE"
            )


class TextInterfaceTests(AccessibilityBridgeTestCase):
    """A text control has the Text interface from the start, labeled or not, empty or not — the object Orca tracks the
    caret in and echoes typing from, so it has to exist before the first keystroke."""

    FIXTURE = "states.html"

    def test_unlabeled_empty_text_input_has_text_interface(self):
        field = wait_for_descendant_by_role(
            self.doc,
            Atspi.Role.TEXT,
            pred=lambda obj: obj.get_name() == "" and get_attributes_dict(obj).get("xml-roles") == "textbox",
        )
        self.assertIsNotNone(field, "states.html must expose its unlabeled text input")
        self.assertTrue(supports_interface(field, "text"), "an unlabeled, empty text input must still have Text")
        self.assertEqual(Atspi.Text.get_character_count(field), 0)

    def test_text_input_text_is_its_value(self):
        field = wait_for_descendant_by_role(self.doc, Atspi.Role.TEXT, name="Readonly text")
        self.assertIsNotNone(field, "states.html must expose its readonly text input")
        self.assertTrue(supports_interface(field, "text"))
        self.assertEqual(Atspi.Text.get_text(field, 0, -1), "readonly value")


if __name__ == "__main__":
    unittest.main()
