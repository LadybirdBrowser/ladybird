"""Lifecycle invariants — page-load focus, AXWebArea presence, basic tree shape."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


class WebAreaPresenceTests(AccessibilityBridgeMacTestCase):
    """The AXWebArea exists and has children after page load."""

    FIXTURE = "roles.html"

    def test_web_area_exists(self):
        """The AXWebArea (document root) is reachable from the application."""
        self.assertIsNotNone(self.web)
        self.assertEqual(_ax_attr(self.web, kAXRoleAttribute), "AXWebArea")

    def test_web_area_has_children(self):
        """The AXWebArea has at least one child accessibility element."""
        children = _ax_attr(self.web, kAXChildrenAttribute) or []
        self.assertGreater(
            len(children),
            0,
            "AXWebArea has no children — accessibility tree did not populate",
        )


if __name__ == "__main__":
    unittest.main()
