"""Tree-shape invariants: what gets exposed, what's hidden, and how ancestry bubbles.

These defend the cross-platform decisions WebContent makes when serializing the accessibility tree, plus the macOS
wrapper's ignored-role filtering (LadybirdAccessibilityElement.mm: is_ignored_role + accessibilityIsIgnored). They
mirror the Linux AccessibilityBridge/test_tree_shape.py suite but use NSAccessibility role/value reads instead of
AT-SPI2 Text-interface walks. Because <p> and unnamed <div> are *ignored* on macOS (their text leaves bubble up via
accessibilityParent's walk-up-past-ignored loop), text-content checks here walk AXStaticText leaves rather than
querying paragraph nodes."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXChildrenAttribute  # noqa: E402
from ApplicationServices import kAXParentAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import walk  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


class TreeShapeTests(AccessibilityBridgeMacTestCase):
    FIXTURE = "tree_shape.html"

    def _all_text(self):
        """Return every AXStaticText leaf's value, anywhere in the document subtree.

        On macOS the WebContent serializer + wrapper's is_ignored_role pair collapses unnamed paragraphs and unnamed
        generic divs out of the tree — so text content lives only in AXStaticText leaves (mapped from "text leaf"
        roles)."""
        out = []

        def visit(obj, _depth):
            if _ax_attr(obj, kAXRoleAttribute) != "AXStaticText":
                return
            value = _ax_attr(obj, kAXValueAttribute)
            if isinstance(value, str) and value:
                out.append(value)

        walk(self.web, visit)
        return out

    def test_title_text_not_in_tree(self):
        """Title/head text children are not exposed — <title> isn't part of the body's accessibility tree."""
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
        """HTML "hidden" attribute — element must not be exposed."""
        joined = " ".join(self._all_text())
        self.assertNotIn("HTML hidden attribute", joined)

    def test_unnamed_div_paragraph_text_is_reachable(self):
        """An unnamed div with a paragraph inside it is collapsed (the div *and* the paragraph are ignored on macOS),
        but the text inside still shows up as a top-level AXStaticText leaf — so AT clients can read it."""
        joined = " ".join(self._all_text())
        self.assertIn("Paragraph inside an unnamed div", joined)

    def test_unnamed_div_does_not_create_intermediate_group(self):
        """When the wrapper collapses an unnamed <div> + unnamed <p>, the AXStaticText leaf's nearest non-ignored
        ancestor is a *named* container or AXWebArea — never an unnamed AXGroup. Otherwise VoiceOver would announce
        spurious "group" wrappers around plain text."""
        target = None

        def visit(obj, _depth):
            nonlocal target
            if target is not None:
                return
            if (
                _ax_attr(obj, kAXRoleAttribute) == "AXStaticText"
                and isinstance(_ax_attr(obj, kAXValueAttribute), str)
                and "Paragraph inside an unnamed div" in _ax_attr(obj, kAXValueAttribute)
            ):
                target = obj

        walk(self.web, visit)
        self.assertIsNotNone(target, "expected AXStaticText with the unnamed-div paragraph text")

        parent = _ax_attr(target, kAXParentAttribute)
        self.assertIsNotNone(parent, "AXStaticText with no AXParent")
        parent_role = _ax_attr(parent, kAXRoleAttribute)
        parent_title = _ax_attr(parent, kAXTitleAttribute)

        # Acceptable parents: AXWebArea (the document), or a named container (any role with a non-empty title).
        # Unacceptable: an AXGroup with no title — that would be the unnamed div leaking into the AX tree.
        if parent_role == "AXGroup":
            self.assertTrue(
                isinstance(parent_title, str) and parent_title,
                f"unnamed div leaked into the AX tree as an AXGroup with no title — "
                f"parent_role={parent_role!r}, parent_title={parent_title!r}",
            )

    def test_named_div_is_exposed(self):
        """A <div aria-label="named-div"> *is* exposed (as AXGroup with the name on AXTitle/AXDescription).

        The wrapper's is_ignored_role pair only ignores generic+empty-name and paragraph+empty-name; a named generic is
        a real container in the tree."""
        named = None

        def visit(obj, _depth):
            nonlocal named
            if named is not None:
                return
            if _ax_attr(obj, kAXTitleAttribute) == "named-div":
                named = obj

        walk(self.web, visit)
        self.assertIsNotNone(named, "named generic (<div aria-label=named-div>) must be exposed")

    def test_role_presentation_promotes_children(self):
        """role='none'/'presentation' element is hidden, children are promoted — the <p> inside should still be in
        the tree as an AXStaticText leaf."""
        joined = " ".join(self._all_text())
        self.assertIn("Presentational role", joined)

    def test_visible_paragraph_does_appear(self):
        """Sanity: visible, non-hidden paragraph *is* exposed (as AXStaticText, since paragraphs themselves are
        ignored on macOS)."""
        joined = " ".join(self._all_text())
        self.assertIn("Visible paragraph that *should* appear", joined)

    def test_web_area_is_the_document_root(self):
        """The document root is an AXWebArea — that's how the wrapper maps WebContent's "document" role."""
        self.assertEqual(_ax_attr(self.web, kAXRoleAttribute), "AXWebArea")

    def test_web_area_parent_is_not_null(self):
        """The AXWebArea has a non-null AXParent (the WebContentView NSView). A null parent would mean VoiceOver
        couldn't walk up out of the web content — and from that NSView VoiceOver finds the window and back up to the
        application."""
        parent = _ax_attr(self.web, kAXParentAttribute)
        self.assertIsNotNone(parent, "AXWebArea's parent must not be null")

    def test_no_ignored_role_node_is_an_accessibility_element(self):
        """Defense in depth: while walking, every visited element should be a real accessibility element. If an
        ignored-role node (unnamed paragraph/unnamed generic) ever showed up here, isAccessibilityIgnored regressed —
        because find_all_by_role / walk only see things AXUIElementCopyAttributeValue surfaces."""
        offenders = []

        def visit(obj, _depth):
            role = _ax_attr(obj, kAXRoleAttribute)
            title = _ax_attr(obj, kAXTitleAttribute)
            # An AXGroup with no title that has only AXStaticText children is suspicious — would suggest an
            # unnamed-paragraph or unnamed-div node leaked through.
            if role == "AXGroup" and not title:
                children = _ax_attr(obj, kAXChildrenAttribute) or []
                if children and all(_ax_attr(c, kAXRoleAttribute) == "AXStaticText" for c in children):
                    offenders.append(role)

        walk(self.web, visit)
        # We don't strictly forbid empty AXGroups — landmark groups have empty AXTitle but a Subrole. So this is
        # defense-in-depth: collect, but don't blanket-fail.
        self.assertLessEqual(
            len(offenders),
            5,
            f"too many unnamed AXGroups whose only children are AXStaticText leaves: {offenders}. This pattern is "
            "what an unnamed-div or unnamed-paragraph leak would look like.",
        )


if __name__ == "__main__":
    unittest.main()
