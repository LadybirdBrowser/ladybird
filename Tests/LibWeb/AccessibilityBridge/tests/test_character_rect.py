"""Per-character layout invariants — guards against regressing a wrapping-text problem.

Key problem to guard against regressing: inline text in a wrapping paragraph should not report y=0 for every character.
Fix walks the containing block's PaintableWithLines."""

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


class CharacterLayoutTests(AccessibilityBridgeTestCase):
    FIXTURE = "paragraphs.html"

    def test_wrapping_paragraph_characters_span_multiple_visual_lines(self):
        """Wrapping paragraph's per-character y increases on new visual lines.

        Previously y=0 for every character on wrapping inline text."""
        ps = find_all_by_role(self.doc, "paragraph")
        wrapping = next(
            (
                p
                for p in ps
                if "deliberately long paragraph" in Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p))
            ),
            None,
        )
        self.assertIsNotNone(wrapping, "no wrapping paragraph in fixture")

        count = Atspi.Text.get_character_count(wrapping)
        ys = set()
        for i in range(count):
            rect = Atspi.Text.get_character_extents(wrapping, i, Atspi.CoordType.SCREEN)
            # Skip "empty" rects (0 w/h) from the line-start-sentinel workaround.
            if rect.width == 0 and rect.height == 0:
                continue
            ys.add(rect.y)
        self.assertGreater(len(ys), 1, f"wrapping paragraph characters must span multiple visual lines; all y={ys}")

    def test_non_wrapping_paragraph_characters_on_single_line(self):
        """Single-line paragraph: all non-empty per-character rects share one y."""
        ps = find_all_by_role(self.doc, "paragraph")
        plain = next(
            (
                p
                for p in ps
                if Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p)).startswith("Plain paragraph")
            ),
            None,
        )
        self.assertIsNotNone(plain)
        count = Atspi.Text.get_character_count(plain)
        ys = set()
        for i in range(count):
            rect = Atspi.Text.get_character_extents(plain, i, Atspi.CoordType.SCREEN)
            if rect.width == 0 and rect.height == 0:
                continue
            ys.add(rect.y)
        self.assertEqual(len(ys), 1, f"non-wrapping paragraph should have a single y; got ys={ys}")

    def test_character_rect_past_end_is_empty(self):
        """characterRect past the end of the text returns an empty rect.

        Works around Qt's GetRangeExtents inclusive-endpoint iteration."""
        ps = find_all_by_role(self.doc, "paragraph")
        plain = next(
            (
                p
                for p in ps
                if Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p)).startswith("Plain paragraph")
            ),
            None,
        )
        self.assertIsNotNone(plain)
        count = Atspi.Text.get_character_count(plain)
        # At exactly count (past the end) and beyond — rect must be empty.
        past_end = Atspi.Text.get_character_extents(plain, count, Atspi.CoordType.SCREEN)
        self.assertEqual(
            (past_end.width, past_end.height),
            (0, 0),
            f"past-end character rect must be empty; got {past_end.width}x{past_end.height}",
        )


if __name__ == "__main__":
    unittest.main()
