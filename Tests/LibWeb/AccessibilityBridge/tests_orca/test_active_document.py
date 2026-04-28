"""Layer-2 tests for Utilities.active_document() and _find_first_content_child().

These are the overrides that let Say All start from the right place when locus of focus is on chrome (e.g., the address
bar right after URL + Enter)."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from harness import LadybirdOrcaTestCase  # noqa: E402


class ActiveDocumentTests(LadybirdOrcaTestCase):
    FIXTURE = "roles.html"

    def setUp(self):
        super().setUp()
        # active_document() consults focus_manager.get_active_window(). In real Orca, that's set by the focus event
        # stream as the user interacts. In our test process, no real focus events flow in — so we prime it here to
        # simulate "user has focused a Ladybird window".
        from orca import focus_manager

        focus_manager.get_manager().set_locus_of_focus(None, self.doc, False)

    def test_active_document_returns_document_web(self):
        """active_document() must resolve to the document web root — whether via EMBEDS or tree search."""
        utils = self.orca.script.utilities
        doc = utils.active_document()
        self.assertIsNotNone(doc, "active_document() returned None")
        self.assertEqual(doc.get_role_name(), "document web")

    def test_find_first_content_child_returns_content(self):
        """_find_first_content_child(doc) returns the first DFS descendant with text or a name."""
        utils = self.orca.script.utilities
        doc = utils.active_document()
        first = utils._find_first_content_child(doc)
        self.assertIsNotNone(first, "first content child is None — Say All would speak nothing")
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi

        # Must have either a non-empty name or non-zero text character count.
        name = first.get_name() or ""
        text_len = Atspi.Text.get_character_count(first) if first else 0
        self.assertTrue(name or text_len, f"first content child has no name and no text: role={first.get_role_name()}")


if __name__ == "__main__":
    unittest.main()
