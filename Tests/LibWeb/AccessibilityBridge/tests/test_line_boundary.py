"""Line-boundary text invariants.

The key problem to prevent regressing: leaf-like containers like list items must return the full flattened text for
textAtOffset(LineBoundary) — not the first visual sub-line.  Paragraphs with wrapping content, by contrast, *do* return
visual sub-lines (flat review depends on this)."""

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


class LeafLikeLineBoundaryTests(AccessibilityBridgeTestCase):
    """Leaf-like containers with LineBoundary must return full flattened text.

    Guards against regressing an I-key problem: on a wrapping list item, Orca calls get_string_at_offset(li, 0, LINE)
    and expects the full item text. A previous buggy implementation returned only the first visual sub-line (often just
    the first character)."""

    FIXTURE = "listitems.html"

    def test_simple_listitem_at_LINE_returns_full_text(self):
        items = find_all_by_role(self.doc, "list item")
        apple = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "Apple"), None
        )
        self.assertIsNotNone(apple, "expected 'Apple' list item")
        r = Atspi.Text.get_string_at_offset(apple, 0, Atspi.TextGranularity.LINE)
        self.assertEqual(r.content, "Apple")
        self.assertEqual(r.start_offset, 0)
        self.assertEqual(r.end_offset, 5)

    def test_wrapping_listitem_at_LINE_returns_full_text_not_first_char(self):
        """Narrow-width listitem whose text wraps one-char-per-line must still speak as 'ABC', not 'A'."""
        items = find_all_by_role(self.doc, "list item")
        abc = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "ABC"), None
        )
        self.assertIsNotNone(abc, "expected 'ABC' narrow list item")
        r = Atspi.Text.get_string_at_offset(abc, 0, Atspi.TextGranularity.LINE)
        self.assertEqual(r.content, "ABC", f"narrow listitem must return full text; got {r.content!r}")
        self.assertEqual(r.start_offset, 0)
        self.assertEqual(r.end_offset, 3)

    def test_listitem_at_LINE_at_every_offset_returns_full_text(self):
        """For a leaf-like container, LINE at any valid offset returns the same whole text."""
        items = find_all_by_role(self.doc, "list item")
        banana = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "Banana"), None
        )
        self.assertIsNotNone(banana)
        length = Atspi.Text.get_character_count(banana)
        for offset in range(length + 1):
            r = Atspi.Text.get_string_at_offset(banana, offset, Atspi.TextGranularity.LINE)
            if r.start_offset == -1:
                continue  # past-end sentinel
            self.assertEqual(r.content, "Banana", f"LINE at offset {offset} must be full text; got {r.content!r}")


class ParagraphLineBoundaryTests(AccessibilityBridgeTestCase):
    """Paragraphs with wrapping content *do* return visual sub-lines.

    This is the other side of the contract: flat review's get_visible_lines iterates per-visual-line — relying on this
    exact behavior for paragraphs."""

    FIXTURE = "paragraphs.html"

    def test_plain_paragraph_at_LINE_returns_full_text(self):
        """Single-line paragraph: the visual line *is* the full text."""
        ps = find_all_by_role(self.doc, "paragraph")
        plain = next(
            (p for p in ps if Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p)).startswith("Plain")), None
        )
        self.assertIsNotNone(plain)
        r = Atspi.Text.get_string_at_offset(plain, 0, Atspi.TextGranularity.LINE)
        self.assertEqual(r.content, "Plain paragraph with no inline objects.")

    def test_wrapping_paragraph_at_LINE_returns_only_first_visual_line(self):
        """Multi-line paragraph — LINE at offset 0 returns only the first visual line, not the full text."""
        ps = find_all_by_role(self.doc, "paragraph")
        wrapping = next(
            (
                p
                for p in ps
                if "deliberately long paragraph" in Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p))
            ),
            None,
        )
        self.assertIsNotNone(wrapping, "expected a visibly-wrapped paragraph fixture")
        full_len = Atspi.Text.get_character_count(wrapping)
        r = Atspi.Text.get_string_at_offset(wrapping, 0, Atspi.TextGranularity.LINE)
        self.assertLess(
            r.end_offset,
            full_len,
            f"wrapping paragraph must return a sub-line, not the whole text "
            f"(got end_offset={r.end_offset}, total={full_len})",
        )
        self.assertEqual(r.start_offset, 0)


if __name__ == "__main__":
    unittest.main()
