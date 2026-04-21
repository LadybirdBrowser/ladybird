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


def _text_of(obj):
    return Atspi.Text.get_text(obj, 0, Atspi.Text.get_character_count(obj))


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

    def test_character_rects_stay_on_their_own_characters_across_collapsed_whitespace(self):
        """Entry N of the per-character geometry is character N, whitespace that renders on no line included.

        The fixture's indented paragraph opens with a newline and an indent that layout collapses, and it wraps, so the
        space at the soft wrap renders on no line either. Neither run may shift the entries that follow it: the last
        character of the first line still ends inside the paragraph on that line, and the first character of the
        second line sits at the paragraph's left edge."""
        ps = find_all_by_role(self.doc, "paragraph")
        indented = next((p for p in ps if "indented source paragraph" in _text_of(p)), None)
        self.assertIsNotNone(indented, "no indented paragraph in fixture")
        text = _text_of(indented)
        first = text.index("An")
        self.assertGreater(first, 0, "the paragraph's leading whitespace should reach the text")
        paragraph = Atspi.Component.get_extents(indented, Atspi.CoordType.SCREEN)

        first_line = Atspi.Text.get_string_at_offset(indented, first, Atspi.TextGranularity.LINE)
        second_line = Atspi.Text.get_string_at_offset(indented, first_line.end_offset, Atspi.TextGranularity.LINE)
        self.assertGreater(second_line.start_offset, first_line.start_offset, "the paragraph should wrap")

        first_rect = Atspi.Text.get_character_extents(indented, first, Atspi.CoordType.SCREEN)
        # The space at the soft wrap renders on no line, so the first line's last rendered character is the one before
        # it. And a line's first character reports no extents on purpose (characterRect() keeps the inclusive iteration
        # of GetRangeExtents off the next line), so the second line is checked at its second character.
        last_on_first_line = Atspi.Text.get_character_extents(
            indented, first_line.end_offset - 2, Atspi.CoordType.SCREEN
        )
        second_on_second_line = Atspi.Text.get_character_extents(
            indented, second_line.start_offset + 1, Atspi.CoordType.SCREEN
        )
        self.assertLessEqual(
            abs(first_rect.x - paragraph.x), 1, f"first character at x={first_rect.x}, paragraph at {paragraph.x}"
        )
        self.assertEqual(last_on_first_line.y, first_rect.y, "the first line's last character left its line")
        self.assertLessEqual(
            last_on_first_line.x + last_on_first_line.width,
            paragraph.x + paragraph.width + 1,
            "the first line's last character ends past the paragraph",
        )
        self.assertGreater(
            second_on_second_line.y, first_rect.y, "the second line's second character isn't below the first line"
        )
        self.assertGreater(
            second_on_second_line.x, paragraph.x, "the second line's second character is at its left edge"
        )
        self.assertLessEqual(
            second_on_second_line.x - paragraph.x,
            2 * first_rect.width + 1,
            f"second line's second character at x={second_on_second_line.x}, paragraph at {paragraph.x}",
        )

    def test_text_after_an_inline_image_sits_to_its_right(self):
        """Bare text after an inline image starts where the image ends, not at the paragraph's left edge: a text leaf's
        character rects come from the text's own box, not its parent element's."""
        ps = find_all_by_role(self.doc, "paragraph")
        tall = next(
            (
                p
                for p in ps
                if Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p)).startswith("Text before a tall")
            ),
            None,
        )
        self.assertIsNotNone(tall, "expected the tall-inline-image paragraph fixture")
        images = find_all_by_role(tall, "image")
        self.assertEqual(len(images), 1, "expected the paragraph's one image")
        image_rect = Atspi.Component.get_extents(images[0], Atspi.CoordType.SCREEN)
        self.assertGreater(image_rect.width, 0)
        full = Atspi.Text.get_text(tall, 0, Atspi.Text.get_character_count(tall))
        after = full.index("inline", full.index("\ufffc"))
        char_rect = Atspi.Text.get_character_extents(tall, after, Atspi.CoordType.SCREEN)
        self.assertGreaterEqual(
            char_rect.x,
            image_rect.x + image_rect.width,
            f"text after the image starts at x={char_rect.x}, left of the image's right edge "
            f"{image_rect.x + image_rect.width}",
        )

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
