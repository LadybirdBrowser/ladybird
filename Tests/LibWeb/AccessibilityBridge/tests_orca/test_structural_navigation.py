"""Layer-2 tests for Orca structural navigation (I, H, K, L keys) on Ladybird.

These tests import Orca into the test process, instantiate our Ladybird Orca script against the live Ladybird AT-SPI2
accessible, capture every speech.speak call, and drive Orca's structural-navigator commands directly. Assertions check
what Orca would have spoken to the user."""

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


class IKeyListItemNavigationTests(LadybirdOrcaTestCase):
    """Structural nav to list items (Orca's I key).

    Guards against regressing a fix for a problem where pressing I on a wrapping listitem would read only the first
    visual sub-line of the listitem — often just the first character."""

    FIXTURE = "listitems.html"

    def _simulate_i_key_on(self, li: Atspi.Accessible):
        """Mimic what Orca's structural_navigator.next_list_item does to announce a listitem.

        The structural navigator ultimately calls script.present_object(li, offset=0, interrupt=True) — which, for list
        items, takes a fast path through web.Script.present_object → default.Script.present_object →
        speech_generator.generate_speech."""
        self.orca.clear_captured()
        self.orca.script.present_object(li, offset=0, interrupt=True)

    def test_simple_listitem_speaks_full_text(self):
        """Orca speaks the full listitem text when no wrapping is involved."""
        items = find_all_by_role(self.doc, "list item")
        apple = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "Apple"),
            None,
        )
        self.assertIsNotNone(apple, "fixture missing 'Apple' listitem")
        self._simulate_i_key_on(apple)
        self.assertIn(
            "Apple", self.orca.captured_text(), f"Orca didn't speak 'Apple'; captured: {self.orca.captured_text()!r}"
        )

    def test_wrapping_listitem_speaks_full_text(self):
        """Narrow-width listitem ('ABC' wrapping one char per line).

        Orca should not speak only 'A' for the whole listitem."""
        items = find_all_by_role(self.doc, "list item")
        abc = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "ABC"),
            None,
        )
        self.assertIsNotNone(abc, "fixture missing narrow 'ABC' listitem")
        self._simulate_i_key_on(abc)
        captured = self.orca.captured_text()
        self.assertIn("ABC", captured, f"Orca spoke {captured!r} instead of including 'ABC'")
        # And it must NOT have spoken just 'A' with nothing else.
        self.assertNotEqual(self.orca.captured_strings(), ["A"], "Orca spoke only 'A'")

    def test_listitem_with_inline_link_speaks_flattened_text(self):
        """Listitem with an inline link speaks its full flattened text."""
        items = find_all_by_role(self.doc, "list item")
        with_link = next(
            (
                li
                for li in items
                if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "See here for details"
            ),
            None,
        )
        self.assertIsNotNone(with_link, "fixture missing 'See here for details' listitem")
        self._simulate_i_key_on(with_link)
        captured = self.orca.captured_text()
        self.assertIn("See here for details", captured, f"expected full flattened text; got {captured!r}")


if __name__ == "__main__":
    unittest.main()
