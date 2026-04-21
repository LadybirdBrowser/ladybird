"""Lifecycle invariants — focus events, tree updates."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from harness import AccessibilityBridgeTestCase  # noqa: E402


class DocumentRootTests(AccessibilityBridgeTestCase):
    """Basic tree-presence invariants: document root exists and is reachable."""

    FIXTURE = "roles.html"

    def test_document_root_has_role_document_web(self):
        """The document web accessible exists with the correct role."""
        self.assertEqual(self.doc.get_role_name(), "document web")

    def test_document_root_parent_is_not_null(self):
        """Document root has a valid parent (the WebContentView accessible)."""
        parent = self.doc.get_parent()
        self.assertIsNotNone(parent)

    def test_document_root_advertises_component(self):
        """Component interface needed for scroll_into_view routing."""
        from harness import supports_interface

        self.assertTrue(supports_interface(self.doc, "component"))


if __name__ == "__main__":
    unittest.main()
