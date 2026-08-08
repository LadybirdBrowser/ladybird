"""Tree-shape invariants: what gets exposed, what's hidden, and how ancestry bubbles."""

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
from harness import walk  # noqa: E402


class TreeShapeTests(AccessibilityBridgeTestCase):
    FIXTURE = "tree_shape.html"

    def _all_text(self):
        """Return every piece of text anywhere in the document subtree."""
        out = []

        def visit(obj, _depth):
            try:
                count = Atspi.Text.get_character_count(obj)
            except Exception:
                return
            if count:
                try:
                    out.append(Atspi.Text.get_text(obj, 0, count))
                except Exception:
                    pass

        walk(self.doc, visit)
        return out

    def test_title_text_not_in_tree(self):
        """Title/head text children are not exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("Title text that should *not* appear", joined)

    def test_script_children_not_in_tree(self):
        """<script> elements and their text are excluded."""
        joined = " ".join(self._all_text())
        self.assertNotIn("no script children either", joined)

    def test_style_children_not_in_tree(self):
        """<style> elements and their text are excluded."""
        joined = " ".join(self._all_text())
        self.assertNotIn("p { color: black", joined)

    def test_display_none_not_in_tree(self):
        """Display:none elements must not be exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("Display none", joined)

    def test_visibility_hidden_not_in_tree(self):
        """Visibility:hidden elements must not be exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("Visibility hidden", joined)

    def test_aria_hidden_not_in_tree(self):
        """aria-hidden='true' elements must not be exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("Aria-hidden", joined)

    def test_html_hidden_attribute_not_in_tree(self):
        """HTML "hidden" attribute — element must not be exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("HTML hidden attribute", joined)

    def test_unnamed_div_is_hidden_but_child_is_reachable(self):
        """Unnamed generic (div) is ignored; its child paragraph bubbles up."""
        paragraphs = find_all_by_role(self.doc, "paragraph")
        joined = " ".join(Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p) or 0) for p in paragraphs)
        self.assertIn("Paragraph inside an unnamed div", joined)
        # No "section" or "generic" accessible exists as ancestor other than the doc + main.
        # Walk: nothing with role "section" should have an empty name and contain our text.
        for p in paragraphs:
            try:
                if "Paragraph inside an unnamed div" in Atspi.Text.get_text(
                    p, 0, Atspi.Text.get_character_count(p) or 0
                ):
                    parent = p.get_parent()
                    # The immediate parent of this paragraph should be the document, *not* an unnamed generic/section.
                    self.assertEqual(
                        parent.get_role_name(),
                        "document web",
                        f"parent role was {parent.get_role_name()!r}, expected document web",
                    )
                    return
            except Exception:
                continue
        self.fail("expected paragraph not found")

    def test_named_div_is_exposed(self):
        """Generic with accessible name *is* exposed."""
        # A section/panel with name 'named-div' should appear.
        named = None

        def visit(obj, _depth):
            nonlocal named
            if named is not None:
                return
            try:
                if obj.get_name() == "named-div":
                    named = obj
            except Exception:
                pass

        walk(self.doc, visit)
        self.assertIsNotNone(named, "named generic (<div aria-label=named-div>) must be exposed")

    def test_role_presentation_promotes_children(self):
        """role='none'/'presentation' element is hidden, children are promoted."""
        paragraphs = find_all_by_role(self.doc, "paragraph")
        joined = " ".join(Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p) or 0) for p in paragraphs)
        self.assertIn("Presentational role", joined)

    def test_visible_paragraph_does_appear(self):
        """Sanity: visible, non-hidden paragraph *is* exposed."""
        joined = " ".join(self._all_text())
        self.assertIn("Visible paragraph that *should* appear", joined)

    def test_document_web_exists_as_single_root(self):
        """The document web exists and is the web root."""
        self.assertEqual(self.doc.get_role_name(), "document web")

    def test_document_parent_is_webcontentview(self):
        """Document root's parent is not null (it's the WebContentView)."""
        parent = self.doc.get_parent()
        self.assertIsNotNone(parent, "document web's parent must not be null")


if __name__ == "__main__":
    unittest.main()
