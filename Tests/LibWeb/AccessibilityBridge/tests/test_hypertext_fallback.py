"""Hypertext-fallback invariants — guards against regressing a Say All link problem.

These invariants are what the Orca-side _install_hypertext_fallback_patch relies on. Specifically, the Nth U+FFFC marker
in a paragraph's text must correspond to its Nth AT-SPI2 exposed child, in DOM order. The problem that motivated this:
When paragraphs contained inline links, Say All silently dropped link text, and jumped link-to-link — never reading
post-link text."""

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


def _full_text(obj):
    try:
        count = Atspi.Text.get_character_count(obj)
    except Exception:
        return ""
    return Atspi.Text.get_text(obj, 0, count) if count else ""


class HypertextAlignmentTests(AccessibilityBridgeTestCase):
    FIXTURE = "paragraphs.html"

    def test_one_link_paragraph_has_one_fffc_and_one_child(self):
        """One U+FFFC ↔ one AT-SPI2 child, DOM-ordered."""
        ps = find_all_by_role(self.doc, "paragraph")
        p = next((p for p in ps if _full_text(p).startswith("A paragraph with")), None)
        self.assertIsNotNone(p)
        text = _full_text(p)
        fffc_count = text.count("￼")
        self.assertEqual(fffc_count, 1)
        self.assertEqual(p.get_child_count(), 1)
        child = p.get_child_at_index(0)
        self.assertEqual(child.get_role_name(), "link")
        self.assertEqual(child.get_name(), "one")

    def test_two_link_paragraph_alignment(self):
        """Two links → two U+FFFC markers → two exposed children in DOM order."""
        ps = find_all_by_role(self.doc, "paragraph")
        p = next((p for p in ps if _full_text(p).startswith("Paragraph with")), None)
        self.assertIsNotNone(p)
        text = _full_text(p)

        fffc_positions = [i for i, c in enumerate(text) if c == "￼"]
        self.assertEqual(len(fffc_positions), 2)
        self.assertEqual(p.get_child_count(), 2)

        # DOM order check.
        names = [p.get_child_at_index(i).get_name() for i in range(p.get_child_count())]
        self.assertEqual(names, ["first", "second"])

        # The positions of the U+FFFC markers must match the text ordering: "Paragraph with U+FFFC and U+FFFC links."
        before_first = text[: fffc_positions[0]]
        between = text[fffc_positions[0] + 1 : fffc_positions[1]]
        after_last = text[fffc_positions[1] + 1 :]
        self.assertEqual(before_first, "Paragraph with ")
        self.assertEqual(between, " and ")
        self.assertEqual(after_last, " links.")


if __name__ == "__main__":
    unittest.main()
