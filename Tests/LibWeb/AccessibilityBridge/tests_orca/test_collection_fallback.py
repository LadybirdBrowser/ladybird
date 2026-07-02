"""Layer-2 tests for the Orca-side Collection fallback.

Our Orca script wraps AXUtilitiesCollection.find_all_with_role (and its role+state variant) with a DFS tree-walk
fallback. This keeps structural nav working on Qt 6.10 — where Collection::GetMatches returns empty. On Qt 6.11 the
original returns real results, and the fallback branch is dormant. Either way, the wrapped function must return correct
matches."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from harness import LadybirdOrcaTestCase  # noqa: E402


class CollectionFallbackTests(LadybirdOrcaTestCase):
    FIXTURE = "roles.html"

    def test_find_all_with_role_returns_headings(self):
        """Three headings in the fixture (h1, h2, h3) must be findable via Collection."""
        import gi

        from orca.ax_utilities_collection import AXUtilitiesCollection

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        results = AXUtilitiesCollection.find_all_with_role(self.doc, [Atspi.Role.HEADING])
        names = {r.get_name() for r in results}
        self.assertIn("First heading", names)
        self.assertIn("Second heading", names)
        self.assertIn("Third heading", names)

    def test_find_all_with_role_returns_links(self):
        """Multiple links in fixture must be findable."""
        import gi

        from orca.ax_utilities_collection import AXUtilitiesCollection

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        results = AXUtilitiesCollection.find_all_with_role(self.doc, [Atspi.Role.LINK])
        self.assertGreater(len(results), 0, "Collection / fallback must find ≥1 link")

    def test_find_all_list_items_via_collection(self):
        """Listitems via collection. Orca's I-key navigation relies on this."""
        from orca.ax_utilities_collection import AXUtilitiesCollection

        results = AXUtilitiesCollection.find_all_list_items(self.doc)
        self.assertGreater(len(results), 0, "no list items found via Collection")


if __name__ == "__main__":
    unittest.main()
