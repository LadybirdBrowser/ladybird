"""Layer-2 tests for the Orca-side hypertext fallback.

Our Orca script installs _install_hypertext_fallback_patch to close four gaps that Qt's bridge leaves in
AtkHypertext/AtkHyperlink. All four are hit by Say All and sentence-extension; misbehavior here shows up as Say All
dropping link text, skipping post-link prose, or jumping link-to-link.

These tests verify the patch produces the right result for the U+FFFC ↔ exposed-child alignment that paragraphs
expose."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import LadybirdOrcaTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402


def _full_text(obj):
    try:
        count = Atspi.Text.get_character_count(obj)
    except Exception:
        return ""
    return Atspi.Text.get_text(obj, 0, count) if count else ""


class HypertextFallbackTests(LadybirdOrcaTestCase):
    FIXTURE = "paragraphs.html"

    def _find_one_link_paragraph(self):
        ps = find_all_by_role(self.doc, "paragraph")
        return next((p for p in ps if _full_text(p).startswith("A paragraph with")), None)

    def _find_two_link_paragraph(self):
        ps = find_all_by_role(self.doc, "paragraph")
        return next((p for p in ps if _full_text(p).startswith("Paragraph with")), None)

    def test_find_child_at_offset_returns_link_at_fffc_position(self):
        """§hypertext fallback: at a U+FFFC offset, find_child_at_offset returns the link."""
        from orca.ax_hypertext import AXHypertext

        p = self._find_one_link_paragraph()
        self.assertIsNotNone(p)
        text = _full_text(p)
        fffc_offset = text.index("￼")

        child = AXHypertext.find_child_at_offset(p, fffc_offset)
        self.assertIsNotNone(child, "find_child_at_offset returned None at U+FFFC — fallback didn't fire")
        self.assertEqual(child.get_role_name(), "link")
        self.assertEqual(child.get_name(), "one")

    def test_find_child_at_offset_returns_none_at_non_fffc(self):
        """find_child_at_offset returns None when offset is NOT at a U+FFFC."""
        from orca.ax_hypertext import AXHypertext

        p = self._find_one_link_paragraph()
        self.assertIsNotNone(p)
        self.assertIsNone(
            AXHypertext.find_child_at_offset(p, 0), "fallback must return None when offset is regular text"
        )

    def test_get_link_start_and_end_offset_correspond_to_fffc_position(self):
        """§hypertext ascent: get_link_start_offset(link) returns the position of its U+FFFC in the parent."""
        from orca.ax_hypertext import AXHypertext

        p = self._find_one_link_paragraph()
        self.assertIsNotNone(p)
        text = _full_text(p)
        fffc_offset = text.index("￼")

        link = p.get_child_at_index(0)
        self.assertEqual(
            AXHypertext.get_link_start_offset(link), fffc_offset, "link start offset must equal its U+FFFC position"
        )
        self.assertEqual(
            AXHypertext.get_link_end_offset(link), fffc_offset + 1, "link end offset must be U+FFFC position + 1"
        )

    def test_get_character_offset_in_parent_matches_fffc_position(self):
        """§hypertext ascent: get_character_offset_in_parent returns the U+FFFC position."""
        from orca.ax_hypertext import AXHypertext

        p = self._find_two_link_paragraph()
        self.assertIsNotNone(p)
        text = _full_text(p)
        fffc_positions = [i for i, c in enumerate(text) if c == "￼"]

        link0 = p.get_child_at_index(0)
        link1 = p.get_child_at_index(1)
        self.assertEqual(AXHypertext.get_character_offset_in_parent(link0), fffc_positions[0])
        self.assertEqual(AXHypertext.get_character_offset_in_parent(link1), fffc_positions[1])


if __name__ == "__main__":
    unittest.main()
