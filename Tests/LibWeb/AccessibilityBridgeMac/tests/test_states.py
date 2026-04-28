"""State + action invariants — AXEnabled, AXFocused, accessibilityActionNames.

The macOS surface is narrower than AT-SPI2 here: no platform-level "focusable" bit (focus is binary AXFocused), and no
"sensitive" state. The wrapper exposes:

* AXEnabled  ←  *not* data->is_disabled
* AXFocused  ←  data->is_focused (or view-is-first-responder, for the document)
* AXPress action  ←  for button/link/checkbox/radio/menuitem/tab roles only"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import AXUIElementCopyActionNames  # noqa: E402
from ApplicationServices import kAXEnabledAttribute  # noqa: E402
from ApplicationServices import kAXFocusedAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


def _action_names(elem):
    """Return the list of action names this element advertises. Empty list on any error."""
    if elem is None:
        return []
    try:
        err, names = AXUIElementCopyActionNames(elem, None)
    except Exception:
        return []
    return list(names) if err == 0 and names else []


def _find_button_by_title(root, title):
    """Locate the button whose AXTitle exactly matches. Returns None if absent."""
    for btn in find_all_by_role(root, "AXButton"):
        if _ax_attr(btn, kAXTitleAttribute) == title:
            return btn
    return None


class EnabledStateTests(AccessibilityBridgeMacTestCase):
    """AXEnabled tracks the wrapper's `!data->is_disabled` rule."""

    FIXTURE = "states.html"

    def test_normal_button_is_enabled(self):
        """A plain <button> reports AXEnabled=YES."""
        btn = wait_for_descendant_by_role(self.web, "AXButton", title="Normal button")
        self.assertIsNotNone(btn, "expected 'Normal button' in the tree")
        self.assertEqual(_ax_attr(btn, kAXEnabledAttribute), True)

    def test_disabled_button_is_not_enabled(self):
        """<button disabled> reports AXEnabled=NO. This is the canary for the wrapper's
        `data->is_disabled ? @NO : @YES` translation."""
        btn = wait_for_descendant_by_role(self.web, "AXButton", title="Disabled button")
        self.assertIsNotNone(btn, "expected 'Disabled button' in the tree")
        self.assertEqual(_ax_attr(btn, kAXEnabledAttribute), False)

    def test_link_is_enabled(self):
        """<a href> reports AXEnabled=YES. (Links don't currently track is_disabled in WebContent.)"""
        link = find_first_by_role(self.web, "AXLink")
        self.assertIsNotNone(link, "expected an AXLink in the tree")
        self.assertEqual(_ax_attr(link, kAXEnabledAttribute), True)

    def test_text_input_is_enabled(self):
        """<input type=text> reports AXEnabled=YES."""
        entry = wait_for_descendant_by_role(self.web, "AXTextField", title="Text field")
        self.assertIsNotNone(entry, "expected the 'Text field' input in the tree")
        self.assertEqual(_ax_attr(entry, kAXEnabledAttribute), True)

    def test_checkbox_is_enabled(self):
        """<input type=checkbox> reports AXEnabled=YES."""
        cb = wait_for_descendant_by_role(self.web, "AXCheckBox", title="Checkbox")
        self.assertIsNotNone(cb, "expected the 'Checkbox' input in the tree")
        self.assertEqual(_ax_attr(cb, kAXEnabledAttribute), True)


class FocusedStateTests(AccessibilityBridgeMacTestCase):
    """AXFocused on individual elements tracks data->is_focused; the document is special-cased to mirror
    view-is-first-responder."""

    FIXTURE = "states.html"

    def test_normal_button_is_not_focused_initially(self):
        """A page-load with no autofocus should have AXFocused=NO on every interactive element. Specifically
        defending the regression where every read of AXFocused returned YES (used to bounce VoiceOver)."""
        btn = wait_for_descendant_by_role(self.web, "AXButton", title="Normal button")
        self.assertIsNotNone(btn)
        # Could be either False or NO (False); the wrapper returns @YES/@NO which PyObjC marshals to True/False.
        self.assertEqual(_ax_attr(btn, kAXFocusedAttribute), False)


class ActionNameTests(AccessibilityBridgeMacTestCase):
    """accessibilityActionNames — read externally via AXUIElementCopyActionNames.

    Wrapper rule: only button/link/checkbox/radio/menuitem/tab advertise AXPress; everything else returns an empty
    action list. Disabled-ness does *not* remove the action from the list. VoiceOver shows greyed-out actions on
    disabled controls — so users know what *would* be available."""

    FIXTURE = "states.html"

    def test_button_advertises_press_action(self):
        """A button exposes AXPress — VoiceOver's VO+Space activation needs this."""
        btn = wait_for_descendant_by_role(self.web, "AXButton", title="Normal button")
        self.assertIsNotNone(btn)
        actions = _action_names(btn)
        self.assertIn("AXPress", actions, f"expected AXPress in button actions, got {actions}")

    def test_link_advertises_press_action(self):
        """A link exposes AXPress."""
        link = find_first_by_role(self.web, "AXLink")
        self.assertIsNotNone(link)
        actions = _action_names(link)
        self.assertIn("AXPress", actions, f"expected AXPress in link actions, got {actions}")

    def test_checkbox_advertises_press_action(self):
        """Checkboxes expose AXPress (toggling them is a press, not a value-set)."""
        cb = wait_for_descendant_by_role(self.web, "AXCheckBox", title="Checkbox")
        self.assertIsNotNone(cb)
        actions = _action_names(cb)
        self.assertIn("AXPress", actions, f"expected AXPress in checkbox actions, got {actions}")

    def test_disabled_button_still_advertises_press_action(self):
        """A disabled button's action list is unchanged. Apple's design pattern is that AXEnabled tells AT clients the
        action is currently unavailable; the action list itself stays stable – so AT can still describe what *would*
        happen."""
        btn = _find_button_by_title(self.web, "Disabled button")
        self.assertIsNotNone(btn)
        actions = _action_names(btn)
        self.assertIn("AXPress", actions, f"disabled button stripped AXPress: got {actions}")

    def test_text_input_does_not_advertise_press_action(self):
        """<input type=text> is not press-able — no AXPress in its action list. VoiceOver drives text fields via
        text-marker navigation, not actions."""
        entry = wait_for_descendant_by_role(self.web, "AXTextField", title="Text field")
        self.assertIsNotNone(entry)
        actions = _action_names(entry)
        self.assertNotIn(
            "AXPress", actions, f"text field shouldn't advertise AXPress (it's not a button/link), got {actions}"
        )


if __name__ == "__main__":
    unittest.main()
