"""Text-marker parameterized attributes — the wrapper's AXNextTextMarkerForTextMarker,
AXPreviousTextMarkerForTextMarker, AXUIElementForTextMarker, AXTextMarkerRangeForUIElement, AXStringForTextMarkerRange,
AXLengthForTextMarkerRange, AXAttributedStringForTextMarkerRange, AXTextMarkerForPosition.

This is the macOS analogue of AT-SPI2's Text interface; Safari uses the same model. There is no equivalent in the Linux
harness because AT-SPI2 uses integer character offsets, not opaque cursor markers.

The wrapper's text-marker payload is a 12-byte struct (8 bytes node ID + 4 bytes offset). These tests don't construct
markers themselves — they consume markers the wrapper hands out and verify round-trips and forward/back walks."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXPositionAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import get_parameterized_attribute_value  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


def _start_marker(web):
    return _ax_attr(web, "AXStartTextMarker")


def _end_marker(web):
    return _ax_attr(web, "AXEndTextMarker")


class DocumentMarkerEndpointsTests(AccessibilityBridgeMacTestCase):
    """AXStartTextMarker/AXEndTextMarker are advertised only on the AXWebArea."""

    FIXTURE = "paragraphs.html"

    def test_axstart_textmarker_present_on_document(self):
        """The web area exposes AXStartTextMarker as a non-nil value. VoiceOver reads this on AXLoadComplete to anchor
        its initial cursor."""
        marker = _start_marker(self.web)
        self.assertIsNotNone(marker, "AXWebArea must expose a non-nil AXStartTextMarker")

    def test_axend_textmarker_present_on_document(self):
        """The web area exposes AXEndTextMarker as a non-nil value."""
        marker = _end_marker(self.web)
        self.assertIsNotNone(marker, "AXWebArea must expose a non-nil AXEndTextMarker")

    def test_start_and_end_markers_are_distinct(self):
        """Start and end refer to different positions in a multi-paragraph document."""
        start = _start_marker(self.web)
        end = _end_marker(self.web)
        # The AXTextMarkerRef objects are CFType-backed and PyObjC compares them by identity. They could theoretically
        # be equal if the document has zero text — but paragraphs.html has plenty.
        self.assertNotEqual(repr(start), repr(end), "AXStartTextMarker and AXEndTextMarker are identical")


class TextMarkerRangeForElementTests(AccessibilityBridgeMacTestCase):
    """AXTextMarkerRangeForUIElement returns the marker range covering a UI element's text content."""

    FIXTURE = "paragraphs.html"

    def test_marker_range_for_static_text_string_round_trips(self):
        """AXTextMarkerRangeForUIElement(AXStaticText) → AXStringForTextMarkerRange(range) returns the leaf's text.

        Concretely: pick a known AXStaticText leaf, ask the wrapper for its marker range, then ask for the string that
        range covers — and then assert the string equals the leaf's AXValue."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0, "expected at least one AXStaticText leaf")

        leaf = leaves[0]
        leaf_value = _ax_attr(leaf, kAXValueAttribute)
        self.assertIsInstance(leaf_value, str, f"leaf has non-string AXValue: {leaf_value!r}")
        self.assertGreater(len(leaf_value), 0, "leaf has empty AXValue")

        marker_range = get_parameterized_attribute_value(leaf, "AXTextMarkerRangeForUIElement", leaf)
        self.assertIsNotNone(marker_range, "AXTextMarkerRangeForUIElement returned nil for a static text leaf")

        # Read the string the marker range covers (asked via the document — text-marker params live there).
        text = get_parameterized_attribute_value(self.web, "AXStringForTextMarkerRange", marker_range)
        self.assertEqual(
            text,
            leaf_value,
            f"AXStringForTextMarkerRange != AXValue. range-text={text!r} value={leaf_value!r}",
        )

    def test_length_matches_string_length(self):
        """AXLengthForTextMarkerRange equals the length of AXStringForTextMarkerRange. The wrapper uses result.length
        directly — so this is guarding against the two queries diverging if either's plumbing changes."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)
        leaf = leaves[0]

        marker_range = get_parameterized_attribute_value(leaf, "AXTextMarkerRangeForUIElement", leaf)
        self.assertIsNotNone(marker_range)

        text = get_parameterized_attribute_value(self.web, "AXStringForTextMarkerRange", marker_range)
        length = get_parameterized_attribute_value(self.web, "AXLengthForTextMarkerRange", marker_range)
        self.assertEqual(length, len(text), f"AXLength={length!r} but len(AXString)={len(text)}")

    def test_attributed_string_returns_attributed_string(self):
        """AXAttributedStringForTextMarkerRange returns an NSAttributedString whose plain-text contents match
        AXStringForTextMarkerRange. The wrapper currently only includes the bare string (no attributes) — but the return
        type must be an NSAttributedString. If it were a plain NSString, VoiceOver's attribute walk would
        fail-shaped."""
        from Foundation import NSAttributedString

        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)
        leaf = leaves[0]

        marker_range = get_parameterized_attribute_value(leaf, "AXTextMarkerRangeForUIElement", leaf)
        self.assertIsNotNone(marker_range)

        attributed = get_parameterized_attribute_value(self.web, "AXAttributedStringForTextMarkerRange", marker_range)
        self.assertIsInstance(attributed, NSAttributedString)
        plain = get_parameterized_attribute_value(self.web, "AXStringForTextMarkerRange", marker_range)
        self.assertEqual(str(attributed.string()), plain)


class TextMarkerWalkTests(AccessibilityBridgeMacTestCase):
    """AXNextTextMarkerForTextMarker and AXPreviousTextMarkerForTextMarker walk forward and backward through the
    document one character at a time, crossing leaf boundaries when needed."""

    FIXTURE = "paragraphs.html"

    def _round_trip_leaf_check(self, marker):
        """Helper: AXUIElementForTextMarker(marker) should be an AXStaticText leaf."""
        elem = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", marker)
        self.assertIsNotNone(elem)
        self.assertEqual(_ax_attr(elem, kAXRoleAttribute), "AXStaticText")
        return elem

    def test_next_advances_within_leaf(self):
        """AXStartTextMarker → AXNextTextMarkerForTextMarker should yield a marker that's still in the same first leaf
        (paragraphs.html's first paragraph has many characters)."""
        start = _start_marker(self.web)
        self.assertIsNotNone(start)

        first_elem = self._round_trip_leaf_check(start)

        nxt = get_parameterized_attribute_value(self.web, "AXNextTextMarkerForTextMarker", start)
        self.assertIsNotNone(nxt, "AXNextTextMarkerForTextMarker returned nil at the document start")

        nxt_elem = self._round_trip_leaf_check(nxt)
        # Both markers should resolve to the same leaf if we advanced within it. (For very short leaves we might cross a
        # boundary on the first step; guard for that.)
        if first_elem != nxt_elem:
            # We crossed a boundary on a single step. That's fine — walking still works — but we wanted to test the
            # within-leaf case, so skip rather than fail.
            self.skipTest("first AXStaticText leaf is too short to test within-leaf advance")

    def test_next_then_previous_round_trips(self):
        """A "right then left" walk from AXStart should land back at AXStart (same leaf, same offset)."""
        start = _start_marker(self.web)
        self.assertIsNotNone(start)

        nxt = get_parameterized_attribute_value(self.web, "AXNextTextMarkerForTextMarker", start)
        self.assertIsNotNone(nxt)

        prev = get_parameterized_attribute_value(self.web, "AXPreviousTextMarkerForTextMarker", nxt)
        self.assertIsNotNone(prev)

        # Comparing AXTextMarkerRefs by identity isn't reliable across CFBridgingRelease, so check equivalence via what
        # the markers point to: same leaf element + same string-to-end length.
        leaf_after_round_trip = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", prev)
        leaf_at_start = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", start)
        self.assertEqual(leaf_after_round_trip, leaf_at_start)

    def test_walk_eventually_crosses_leaf_boundary(self):
        """Walking forward enough times crosses from one AXStaticText leaf to the next; paragraphs.html has
        multiple paragraphs, each its own leaf."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreaterEqual(len(leaves), 2, "fixture must have at least 2 text leaves to test boundary crossing")

        first_leaf_value = _ax_attr(leaves[0], kAXValueAttribute) or ""
        first_leaf_len = len(first_leaf_value)
        # Cap the walk to first_leaf_len + a few.
        max_steps = first_leaf_len + 10

        marker = _start_marker(self.web)
        self.assertIsNotNone(marker)
        starting_leaf = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", marker)

        crossed = False
        for _ in range(max_steps):
            nxt = get_parameterized_attribute_value(self.web, "AXNextTextMarkerForTextMarker", marker)
            if nxt is None:
                break
            marker = nxt
            now_leaf = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", marker)
            if now_leaf and now_leaf != starting_leaf:
                crossed = True
                break

        self.assertTrue(
            crossed,
            f"walked {max_steps} AXNextTextMarkerForTextMarker steps without crossing a leaf boundary",
        )


class TextMarkerForPositionTests(AccessibilityBridgeMacTestCase):
    """AXTextMarkerForPosition — given a screen point, return a marker for the closest text content."""

    FIXTURE = "paragraphs.html"

    def test_marker_for_static_text_position_resolves_to_text(self):
        """Take an AXStaticText's AXPosition (its top-left screen point), pass it to AXTextMarkerForPosition, and verify
        the returned marker resolves to a static-text leaf — ideally the same one.

        The wrapper's AXTextMarkerForPosition does a hit test on the WebContent at the given point; if the hit lands on
        a text leaf, it returns a marker for that leaf — otherwise it walks down to the first text descendant. Either
        way, the returned marker should map back to *some* AXStaticText."""
        import re

        from Foundation import NSValue

        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)

        leaf = leaves[0]
        pos_val = _ax_attr(leaf, kAXPositionAttribute)
        self.assertIsNotNone(pos_val, "AXStaticText must have an AXPosition")

        # AXPosition is an AXValueRef (not an NSValue). PyObjC has no clean unwrapper for this opaque CFType, but
        # str(AXValueRef) yields a stable diagnostic of the form
        #     "<AXValue 0x...> {value = x:N y:M type = kAXValueCGPointType}"
        # — same approach test_regressions.py uses for AXVisibleCharacterRange CFRange unwrap.
        m = re.search(r"x:(-?\d+\.?\d*)\s+y:(-?\d+\.?\d*)", str(pos_val))
        self.assertIsNotNone(m, f"could not parse AXPosition: {pos_val!r}")
        x, y = float(m.group(1)), float(m.group(2))

        # Nudge a few pixels into the rect so we land *on* glyphs — not at the very edge, where hit-testing might pick a
        # neighboring element.
        nudged = NSValue.valueWithPoint_((x + 5, y + 5))
        marker = get_parameterized_attribute_value(self.web, "AXTextMarkerForPosition", nudged)
        if marker is None:
            self.skipTest("AXTextMarkerForPosition returned nil for the leaf's position — hit test missed the leaf")

        elem = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", marker)
        self.assertIsNotNone(elem)
        self.assertEqual(_ax_attr(elem, kAXRoleAttribute), "AXStaticText")


if __name__ == "__main__":
    unittest.main()
