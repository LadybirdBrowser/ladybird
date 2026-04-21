"""AT-SPI2 interface advertisement — which accessibles expose which interfaces."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from gi.repository import GLib  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import supports_interface  # noqa: E402


class TextInterfaceAdvertisementTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def test_paragraph_advertises_text(self):
        """Paragraphs advertise Text. Orca's is_text_block_element needs it."""
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0)
        for p in ps:
            self.assertTrue(supports_interface(p, "text"))

    def test_list_does_not_advertise_text(self):
        """<ul>/<ol> (role 'list') must *not* advertise Text.

        Protects against a known pitfall: a list's Text would be '[U+FFFC][U+FFFC][U+FFFC]…' for each listitem — and
        without a Hypertext interface, Orca would skip the entire list."""
        lsts = find_all_by_role(self.doc, "list")
        self.assertGreater(len(lsts), 0)
        for lst in lsts:
            self.assertFalse(supports_interface(lst, "text"), "list must *not* advertise Text")

    def test_link_advertises_text(self):
        """Leaf-like containers advertise Text (link case)."""
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link)
        self.assertTrue(supports_interface(link, "text"))

    def test_button_advertises_text(self):
        """Leaf-like containers advertise Text (button case)."""
        btn = find_first_by_role(self.doc, Atspi.Role.PUSH_BUTTON)
        self.assertIsNotNone(btn)
        self.assertTrue(supports_interface(btn, "text"))

    def test_heading_advertises_text(self):
        """Leaf-like containers advertise Text (heading case)."""
        h = find_first_by_role(self.doc, "heading")
        self.assertIsNotNone(h)
        self.assertTrue(supports_interface(h, "text"))

    def test_image_advertises_text(self):
        """Image with alt text advertises Text so Orca can read the alt."""
        img = find_first_by_role(self.doc, "image")
        self.assertIsNotNone(img)
        self.assertTrue(supports_interface(img, "text"))

    def test_listitem_advertises_text(self):
        """Listitems advertise Text."""
        li = find_first_by_role(self.doc, "list item")
        self.assertIsNotNone(li)
        self.assertTrue(supports_interface(li, "text"))

    def test_named_section_advertises_text(self):
        """A named, childless generic container (via aria-label) advertises Text.

        The bridge grants Text to a named node with no children (interface_cast: !name.is_empty() && childCount() == 0),
        so its accessible name is reachable as text."""
        from harness import walk

        named = None

        def visit(obj, _depth):
            nonlocal named
            if named is not None:
                return
            try:
                name = obj.get_name()
            except GLib.GError:
                return
            if name == "Empty named section":
                named = obj

        walk(self.doc, visit)
        self.assertIsNotNone(named, "expected the empty named section in the accessibility tree")
        self.assertEqual(named.get_child_count(), 0)
        self.assertTrue(supports_interface(named, "text"))

    def test_unnamed_container_with_direct_text_advertises_text(self):
        """An unnamed container with direct text exposes that text exactly once.

        collect_exposed_children hides a container's text-leaf children because they are reachable through the
        container's own hypertext. An unnamed article, blockquote, or section is not a text leaf, a leaf-like role, a
        paragraph, or named, so without granting it Text its direct text would be exposed nowhere. The bridge grants
        Text when build_hypertext() yields real text rather than only U+FFFC markers."""
        from harness import walk

        matches = []

        def visit(obj, _depth):
            if not supports_interface(obj, "text"):
                return
            try:
                count = Atspi.Text.get_character_count(obj)
                content = Atspi.Text.get_text(obj, 0, count)
            except GLib.GError:
                return
            if "Blockquote sidebar remark." in content:
                matches.append(obj)

        walk(self.doc, visit)
        self.assertEqual(
            len(matches),
            1,
            "the unnamed container's direct text must be exposed exactly once (its own Text)",
        )
        self.assertEqual(matches[0].get_child_count(), 0, "the text-leaf child must stay hidden")


class ActionInterfaceAdvertisementTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def test_button_advertises_action(self):
        """Buttons expose Action interface with Press."""
        btn = find_first_by_role(self.doc, Atspi.Role.PUSH_BUTTON)
        self.assertIsNotNone(btn)
        self.assertTrue(supports_interface(btn, "action"))
        # Check action names
        try:
            n = Atspi.Action.get_n_actions(btn)
        except Exception:
            n = 0
        names = [Atspi.Action.get_action_name(btn, i).lower() for i in range(n)]
        has_press = any("press" in name or "click" in name for name in names)
        self.assertTrue(has_press, f"button must advertise a press/click action; got {names}")

    def test_link_advertises_action(self):
        """Links expose Action (activation)."""
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link)
        self.assertTrue(supports_interface(link, "action"))


if __name__ == "__main__":
    unittest.main()
