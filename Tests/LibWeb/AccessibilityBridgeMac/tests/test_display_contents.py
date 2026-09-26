"""display:contents elements: which ones the tree keeps, and the frame a kept one reports.

A display:contents element has no layout box of its own; its children are laid out in its parent's box. WebContent
still includes it under the usual rules (Element::exclude_from_accessibility_tree() exempts it from the no-layout-node
exclusion), and serializes its bounds as the union of its children's boxes (AccessibilityTreeNode.cpp:
accessibility_bounds()). So an unnamed display:contents div collapses out of the macOS tree like any unnamed div, while
a display:contents element with a role and a name is exposed with a frame VoiceOver can point at."""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXParentAttribute  # noqa: E402
from ApplicationServices import kAXPositionAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXSizeAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import walk  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


def _rect(obj):
    """(x, y, width, height) of obj in screen points, read from its AXPosition and AXSize.

    Both are AXValueRefs, which PyObjC can't unwrap; str() of one is a stable diagnostic of the form
    "<AXValue 0x...> {value = x:N y:M type = kAXValueCGPointType}", as test_text_markers.py reads too."""
    position = re.search(r"x:(-?\d+\.?\d*)\s+y:(-?\d+\.?\d*)", str(_ax_attr(obj, kAXPositionAttribute)))
    size = re.search(r"w:(-?\d+\.?\d*)\s+h:(-?\d+\.?\d*)", str(_ax_attr(obj, kAXSizeAttribute)))
    if position is None or size is None:
        return None
    return tuple(float(v) for v in (position.group(1), position.group(2), size.group(1), size.group(2)))


class DisplayContentsTests(AccessibilityBridgeMacTestCase):
    FIXTURE = "display_contents.html"

    def _button_titled(self, title):
        for button in find_all_by_role(self.web, "AXButton"):
            if _ax_attr(button, kAXTitleAttribute) == title:
                return button
        self.fail(f"no AXButton titled {title!r}")

    def _group_titled(self, title):
        found = None

        def visit(obj, _depth):
            nonlocal found
            if (
                found is None
                and _ax_attr(obj, kAXRoleAttribute) == "AXGroup"
                and _ax_attr(obj, kAXTitleAttribute) == title
            ):
                found = obj

        walk(self.web, visit)
        self.assertIsNotNone(found, f"no AXGroup titled {title!r}")
        return found

    def test_unnamed_display_contents_div_collapses(self):
        """The button inside an unnamed display:contents div hangs off the AXWebArea, not off an untitled AXGroup —
        the div is a nameless generic, and the wrapper ignores those whether or not they have a box."""
        button = self._button_titled("Contents button")
        parent = _ax_attr(button, kAXParentAttribute)
        self.assertIsNotNone(parent)
        self.assertEqual(_ax_attr(parent, kAXRoleAttribute), "AXWebArea")

    def test_named_display_contents_group_is_exposed_with_its_children(self):
        """A display:contents element with role=group and a name is in the tree, and its buttons are its children."""
        group = self._group_titled("Contents group")
        child_titles = [_ax_attr(child, kAXTitleAttribute) for child in _ax_attr(group, kAXChildrenAttribute) or []]
        self.assertEqual(child_titles, ["First grouped button", "Second grouped button"])

    def test_named_display_contents_group_frame_spans_its_children(self):
        """The group has no box of its own, so its frame is the union of its children's frames — not an empty rect
        at the origin, which would put VoiceOver's cursor nowhere."""
        group = self._group_titled("Contents group")
        first = _rect(self._button_titled("First grouped button"))
        second = _rect(self._button_titled("Second grouped button"))
        self.assertIsNotNone(first)
        self.assertIsNotNone(second)
        left = min(first[0], second[0])
        top = min(first[1], second[1])
        right = max(first[0] + first[2], second[0] + second[2])
        bottom = max(first[1] + first[3], second[1] + second[3])
        self.assertEqual(_rect(group), (left, top, right - left, bottom - top))


if __name__ == "__main__":
    unittest.main()
