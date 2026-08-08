"""Hypertext-model text content invariants."""

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
from harness import find_first_by_role  # noqa: E402
from harness import supports_interface  # noqa: E402


def _full_text(obj):
    """Return the accessible's full Atspi.Text content, or empty string."""
    try:
        count = Atspi.Text.get_character_count(obj)
    except Exception:
        return ""
    if not count:
        return ""
    try:
        return Atspi.Text.get_text(obj, 0, count)
    except Exception:
        return ""


class HypertextContentTests(AccessibilityBridgeTestCase):
    FIXTURE = "paragraphs.html"

    def _paragraphs(self):
        return find_all_by_role(self.doc, "paragraph")

    def test_plain_paragraph_text(self):
        """Paragraph with no embedded objects contains no U+FFFC."""
        ps = self._paragraphs()
        plain = next((p for p in ps if _full_text(p).startswith("Plain paragraph")), None)
        self.assertIsNotNone(plain, "could not find the plain paragraph fixture")
        text = _full_text(plain)
        self.assertEqual(text, "Plain paragraph with no inline objects.")
        self.assertNotIn("￼", text)

    def test_paragraph_with_one_link_has_one_fffc(self):
        """Paragraph with one inline link has exactly one U+FFFC where the link goes."""
        ps = self._paragraphs()
        p = next((p for p in ps if _full_text(p).startswith("A paragraph with")), None)
        self.assertIsNotNone(p, "could not find the one-link paragraph")
        text = _full_text(p)
        self.assertEqual(text.count("￼"), 1, f"expected exactly one U+FFFC, got {text!r}")
        # The order around the U+FFFC should match the DOM order.
        self.assertTrue(text.startswith("A paragraph with "))
        self.assertTrue(text.endswith(" link embedded."))

    def test_paragraph_with_two_links_has_two_fffcs(self):
        """Two inline links in one paragraph → two U+FFFC markers."""
        ps = self._paragraphs()
        p = next((p for p in ps if _full_text(p).startswith("Paragraph with")), None)
        self.assertIsNotNone(p, "could not find the two-link paragraph")
        text = _full_text(p)
        self.assertEqual(text.count("￼"), 2, f"expected two U+FFFC markers, got {text!r}")

    def test_paragraph_characterCount_matches_text_length(self):
        """Character_count == len(get_text(0, -1)) including U+FFFC markers."""
        for p in self._paragraphs():
            count = Atspi.Text.get_character_count(p)
            text = _full_text(p)
            self.assertEqual(
                count, len(text), f"paragraph text {text!r} has character_count {count} != len {len(text)}"
            )

    def test_paragraph_advertises_text_interface(self):
        """Paragraphs advertise Text interface — required by Orca's is_text_block_element."""
        for p in self._paragraphs():
            self.assertTrue(
                supports_interface(p, "text"),
                f"paragraph must advertise Text; interfaces: {Atspi.Accessible.get_interfaces(p)}",
            )

    def test_nth_fffc_corresponds_to_nth_exposed_child(self):
        """Nth U+FFFC marker in paragraph text corresponds to its Nth exposed child.

        This is what makes the Orca-side hypertext fallback work."""
        ps = self._paragraphs()
        two_link = next((p for p in ps if _full_text(p).startswith("Paragraph with")), None)
        self.assertIsNotNone(two_link)

        text = _full_text(two_link)
        fffc_positions = [i for i, c in enumerate(text) if c == "￼"]
        child_count = two_link.get_child_count()
        self.assertEqual(
            len(fffc_positions), child_count, f"#U+FFFC ({len(fffc_positions)}) must equal #children ({child_count})"
        )

        # And the children must be in DOM order: first link named 'first', second named 'second'.
        first_child = two_link.get_child_at_index(0)
        self.assertEqual(first_child.get_name(), "first")
        second_child = two_link.get_child_at_index(1)
        self.assertEqual(second_child.get_name(), "second")


class ListHypertextTests(AccessibilityBridgeTestCase):
    FIXTURE = "listitems.html"

    def _listitems(self):
        return find_all_by_role(self.doc, "list item")

    def test_listitem_flattens_descendant_text(self):
        """Listitem with inline link exposes flattened text, no U+FFFC."""
        items = self._listitems()
        with_link = next((li for li in items if _full_text(li) == "See here for details"), None)
        self.assertIsNotNone(with_link, "expected the 'See here for details' list item")
        text = _full_text(with_link)
        self.assertEqual(text, "See here for details")
        self.assertNotIn("￼", text)

    def test_list_does_not_advertise_text_interface(self):
        """<ul>/<ol> (role 'list') must *not* advertise Text — would stringify as [U+FFFC][U+FFFC][U+FFFC]…"""
        lst = find_first_by_role(self.doc, "list")
        self.assertIsNotNone(lst)
        self.assertFalse(supports_interface(lst, "text"), "list must *not* advertise Text interface")

    def test_listitem_advertises_text_interface(self):
        """Listitems advertise Text."""
        for li in self._listitems():
            self.assertTrue(supports_interface(li, "text"))

    def test_listitem_link_still_exposed_as_child(self):
        """Embedded objects (links) remain AT-SPI2 children even when text is flattened."""
        items = self._listitems()
        with_link = next((li for li in items if _full_text(li) == "See here for details"), None)
        self.assertIsNotNone(with_link)
        # Exactly one child (the link), not three (link + the two text leaves around it).
        self.assertEqual(with_link.get_child_count(), 1)
        link = with_link.get_child_at_index(0)
        self.assertEqual(link.get_role_name(), "link")


if __name__ == "__main__":
    unittest.main()
