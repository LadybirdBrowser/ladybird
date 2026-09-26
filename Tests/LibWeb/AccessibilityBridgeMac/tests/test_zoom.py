"""Page zoom: the frames the bridge reports follow the zoomed page.

WebContent lays the page out in CSS pixels whatever the zoom level, and folds the zoom into its device scale (the way
Gecko does: nsPresContext::SetFullZoom() refreshes the ratio LocalAccessible::Bounds() converts with), so the bounds it
serializes stay in CSS pixels while the view draws every one of them zoom_level() points wide. The AppKit bridge scales
them on the way out (LadybirdWebView.mm: accessibilityScreenRectForViewRect:), so after a Zoom In, VoiceOver's cursor
lands on the text where it's drawn, not where it was at 100%."""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import AXUIElementPerformAction  # noqa: E402
from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXMenuBarAttribute  # noqa: E402
from ApplicationServices import kAXPositionAttribute  # noqa: E402
from ApplicationServices import kAXPressAction  # noqa: E402
from ApplicationServices import kAXSizeAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import wait_for  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402

# ViewImplementation::ZOOM_STEP: one Zoom In takes the page from 100% to 110%.
ZOOM_IN_FACTOR = 1.1
PLAIN_PARAGRAPH = "Plain paragraph with no inline objects."


def _rect(obj):
    """(x, y, width, height) of obj in screen points, read from its AXPosition and AXSize.

    Both are AXValueRefs, which PyObjC can't unwrap; str() of one is a stable diagnostic of the form
    "<AXValue 0x...> {value = x:N y:M type = kAXValueCGPointType}", as test_display_contents.py reads too."""
    position = re.search(r"x:(-?\d+\.?\d*)\s+y:(-?\d+\.?\d*)", str(_ax_attr(obj, kAXPositionAttribute)))
    size = re.search(r"w:(-?\d+\.?\d*)\s+h:(-?\d+\.?\d*)", str(_ax_attr(obj, kAXSizeAttribute)))
    if position is None or size is None:
        return None
    return tuple(float(v) for v in (position.group(1), position.group(2), size.group(1), size.group(2)))


class ZoomTests(AccessibilityBridgeMacTestCase):
    FIXTURE = "paragraphs.html"

    def _menu_item(self, *titles):
        """The AXMenuItem at this path of titles under the menu bar, e.g. ("View", "Zoom", "Zoom In")."""
        menu_bar = _ax_attr(self.app, kAXMenuBarAttribute)
        self.assertIsNotNone(menu_bar, "the application has no AXMenuBar")
        candidates = _ax_attr(menu_bar, kAXChildrenAttribute) or []
        item = None
        for depth, title in enumerate(titles):
            if depth:
                # An item that opens a submenu has that AXMenu as its one child; the submenu's items are its children.
                candidates = [
                    child
                    for menu in _ax_attr(item, kAXChildrenAttribute) or []
                    for child in _ax_attr(menu, kAXChildrenAttribute) or []
                ]
            item = next(
                (candidate for candidate in candidates if _ax_attr(candidate, kAXTitleAttribute) == title), None
            )
            self.assertIsNotNone(item, f"no {title!r} item under {' > '.join(titles[:depth]) or 'the menu bar'}")
        return item

    def _press_menu_item(self, *titles):
        error = AXUIElementPerformAction(self._menu_item(*titles), kAXPressAction)
        self.assertEqual(error, 0, f"AXPress on {' > '.join(titles)} failed with AXError {error}")

    def _plain_paragraph_rect(self):
        for leaf in find_all_by_role(self.web, "AXStaticText"):
            if _ax_attr(leaf, kAXValueAttribute) == PLAIN_PARAGRAPH:
                return _rect(leaf)
        return None

    def test_frames_scale_with_the_page_zoom(self):
        """After View > Zoom > Zoom In, the plain paragraph's text is 1.1 times as wide and as tall, and 1.1 times as far
        from the web area's top-left corner, as it was at 100% — the CSS-pixel bounds haven't moved, the view draws
        them zoomed, and the bridge reports them where they're drawn. Reset Zoom puts the frame back."""
        web_area = _rect(self.web)
        before = self._plain_paragraph_rect()
        self.assertIsNotNone(web_area, "no frame for the AXWebArea")
        self.assertIsNotNone(before, "no AXStaticText for the plain paragraph")

        self._press_menu_item("View", "Zoom", "Zoom In")
        try:
            changed = wait_for(
                lambda: self._plain_paragraph_rect() not in (None, before),
                description="the plain paragraph's frame to change after Zoom In",
            )
            self.assertTrue(changed, f"the plain paragraph's frame stayed at {before} after Zoom In")
            after = self._plain_paragraph_rect()
            self.assertIsNotNone(after, "the plain paragraph's frame went missing after Zoom In")
            for axis, offset_index, size_index in (("x", 0, 2), ("y", 1, 3)):
                offset_before = before[offset_index] - web_area[offset_index]
                offset_after = after[offset_index] - web_area[offset_index]
                self.assertAlmostEqual(
                    offset_after,
                    offset_before * ZOOM_IN_FACTOR,
                    delta=1.0,
                    msg=f"{axis} offset from the web area: {offset_before} at 100%, {offset_after} after Zoom In",
                )
                self.assertAlmostEqual(
                    after[size_index],
                    before[size_index] * ZOOM_IN_FACTOR,
                    delta=1.0,
                    msg=f"size along {axis}: {before[size_index]} at 100%, {after[size_index]} after Zoom In",
                )
        finally:
            self._press_menu_item("View", "Zoom", "Reset Zoom")

        restored = wait_for(
            lambda: self._plain_paragraph_rect() == before,
            description="the plain paragraph's frame to return after Reset Zoom",
        )
        self.assertTrue(
            restored, f"the plain paragraph's frame is {self._plain_paragraph_rect()} after Reset Zoom, not {before}"
        )


if __name__ == "__main__":
    unittest.main()
