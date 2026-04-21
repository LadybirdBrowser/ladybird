"""Page zoom: the extents the bridge reports follow the zoomed page.

WebContent lays the page out in CSS pixels whatever the zoom level, and folds the zoom into its device scale (the way
Gecko does: nsPresContext::SetFullZoom() refreshes the ratio LocalAccessible::Bounds() converts with), so the bounds it
serializes stay in CSS pixels while the view draws every one of them zoom_level() times as large. The Qt bridge scales
them on the way out (AccessibilityInterface.cpp: css_rect_to_global()), so after a Zoom In, Orca's character extents
land on the text where it's drawn, not where it was at 100%."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import dump_subtree  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import wait_for  # noqa: E402

# ViewImplementation::ZOOM_STEP: one Zoom In takes the page from 100% to 110%.
ZOOM_IN_FACTOR = 1.1
PLAIN_PARAGRAPH = "Plain paragraph with no inline objects."


def _text_of(obj):
    return Atspi.Text.get_text(obj, 0, Atspi.Text.get_character_count(obj))


class ZoomTests(AccessibilityBridgeTestCase):
    FIXTURE = "paragraphs.html"

    def _menu_item(self, name):
        """The menu item named name anywhere in the application. The Qt menu bar is in the AT-SPI tree whether or not
        the window shows it, and pressing one of its items triggers the action either way."""
        for item in find_all_by_role(self.app, Atspi.Role.MENU_ITEM):
            if item.get_name() == name:
                return item
        self.fail(f"no menu item named {name!r}; application tree:\n{dump_subtree(self.app, max_depth=4)}")

    def _press_menu_item(self, name):
        item = self._menu_item(name)
        names = [Atspi.Action.get_action_name(item, i) for i in range(Atspi.Action.get_n_actions(item))]
        self.assertTrue(names, f"menu item {name!r} has no actions")
        self.assertTrue(Atspi.Action.do_action(item, 0), f"action {names[0]!r} on menu item {name!r} failed")

    def _plain_paragraph(self):
        for paragraph in find_all_by_role(self.doc, "paragraph"):
            if _text_of(paragraph) == PLAIN_PARAGRAPH:
                return paragraph
        self.fail(f"no paragraph reads {PLAIN_PARAGRAPH!r}")

    def _text_extents(self):
        """(x, y, width, height) of the plain paragraph's text in screen pixels: from its first character's rect to
        its last character's. The paragraph's own extents are no use here — a block is as wide as the viewport, and
        the viewport is as wide as the widget whatever the zoom."""
        paragraph = self._plain_paragraph()
        first = Atspi.Text.get_character_extents(paragraph, 0, Atspi.CoordType.SCREEN)
        last = Atspi.Text.get_character_extents(paragraph, len(PLAIN_PARAGRAPH) - 1, Atspi.CoordType.SCREEN)
        return (first.x, first.y, last.x + last.width - first.x, first.height)

    def test_extents_scale_with_the_page_zoom(self):
        """After Zoom In, the plain paragraph's text is 1.1 times as wide and as tall, and 1.1 times as far from the
        document's top-left corner, as it was at 100% — the CSS-pixel bounds haven't moved, the view draws them
        zoomed, and the bridge reports them where they're drawn. Reset Zoom puts the extents back."""
        document = Atspi.Component.get_extents(self.doc, Atspi.CoordType.SCREEN)
        origin = (document.x, document.y)
        before = self._text_extents()

        self._press_menu_item("Zoom In")
        try:
            changed = wait_for(
                lambda: self._text_extents() != before,
                description="the plain paragraph's text extents to change after Zoom In",
            )
            self.assertTrue(changed, f"the plain paragraph's text extents stayed at {before} after Zoom In")
            after = self._text_extents()
            for axis, offset_index, size_index in (("x", 0, 2), ("y", 1, 3)):
                offset_before = before[offset_index] - origin[offset_index]
                offset_after = after[offset_index] - origin[offset_index]
                self.assertAlmostEqual(
                    offset_after,
                    offset_before * ZOOM_IN_FACTOR,
                    delta=1.5,
                    msg=f"{axis} offset from the document: {offset_before} at 100%, {offset_after} after Zoom In",
                )
                self.assertAlmostEqual(
                    after[size_index],
                    before[size_index] * ZOOM_IN_FACTOR,
                    delta=1.5,
                    msg=f"size along {axis}: {before[size_index]} at 100%, {after[size_index]} after Zoom In",
                )
        finally:
            self._press_menu_item("Reset Zoom")

        restored = wait_for(
            lambda: self._text_extents() == before,
            description="the plain paragraph's text extents to return after Reset Zoom",
        )
        self.assertTrue(restored, f"the text extents are {self._text_extents()} after Reset Zoom, not {before}")


if __name__ == "__main__":
    unittest.main()
