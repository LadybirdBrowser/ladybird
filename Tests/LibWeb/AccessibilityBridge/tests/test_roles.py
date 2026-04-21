"""Role-mapping invariants"""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi  # noqa: E402

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import get_attributes_dict  # noqa: E402
from harness import wait_for_all_by_role  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402


class RoleMappingTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def test_heading_roles(self):
        """<h1>..<h3> all map to "heading" with correct "level" attribute."""
        headings = wait_for_all_by_role(self.doc, "heading")
        self.assertGreaterEqual(len(headings), 3, f"expected ≥3 headings, got {len(headings)}")

        by_level = {}
        for h in headings:
            attrs = get_attributes_dict(h)
            level = attrs.get("level")
            self.assertIsNotNone(level, f"heading {h.get_name()!r} has no 'level' attribute")
            by_level[level] = h.get_name()

        self.assertEqual(by_level.get("1"), "First heading")
        self.assertEqual(by_level.get("2"), "Second heading")
        self.assertEqual(by_level.get("3"), "Third heading")

    def test_paragraph_role(self):
        """<p> maps to "paragraph" — not "section". Orca's Say All sentence extension needs this."""
        ps = wait_for_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0, "expected at least one paragraph accessible")

    def test_button_role(self):
        """<button> maps to Atspi.Role.PUSH_BUTTON."""
        btn = wait_for_descendant_by_role(self.doc, Atspi.Role.PUSH_BUTTON)
        self.assertIsNotNone(btn)
        self.assertEqual(btn.get_name(), "A button")

    def test_link_role(self):
        """<a href> maps to "link"."""
        links = wait_for_all_by_role(self.doc, "link")
        names = [link.get_name() for link in links]
        self.assertIn("Standalone link", names)

    def test_image_role(self):
        """<img alt> maps to "image"."""
        img = wait_for_descendant_by_role(self.doc, "image")
        self.assertIsNotNone(img)
        self.assertEqual(img.get_name(), "An image")

    def test_list_role(self):
        """<ul> maps to "list"."""
        lst = wait_for_descendant_by_role(self.doc, "list")
        self.assertIsNotNone(lst)

    def test_listitem_role(self):
        """<li> maps to "list item"."""
        li = wait_for_descendant_by_role(self.doc, "list item")
        self.assertIsNotNone(li)
        # Name is empty (text content comes through Atspi.Text, not the accessible name).
        import gi

        gi.require_version("Atspi", "2.0")
        from gi.repository import Atspi  # noqa

        text = Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li))
        self.assertIn("List item", text)

    def test_xml_roles_landmarks(self):
        """Landmark elements expose "xml-roles" matching their ARIA role."""
        # Walk and collect xml-roles values
        from harness import walk

        by_xml_role = {}

        def visit(obj, _depth):
            attrs = get_attributes_dict(obj)
            xr = attrs.get("xml-roles", "")
            if xr:
                by_xml_role.setdefault(xr, []).append(obj.get_name())

        walk(self.doc, visit)

        self.assertIn("navigation", by_xml_role, f"expected xml-roles:navigation somewhere; got {sorted(by_xml_role)}")
        self.assertIn("main", by_xml_role)
        # A <header> at the top level computes to "banner".
        self.assertIn("banner", by_xml_role)
        # A top-level <footer> computes to "contentinfo".
        self.assertIn("contentinfo", by_xml_role)

    def test_tag_attribute_present(self):
        """Every exposed node with a role has a "tag" attribute."""
        from harness import walk

        missing = []

        def visit(obj, _depth):
            role = obj.get_role_name()
            if role in ("filler", "invalid", ""):
                return
            attrs = get_attributes_dict(obj)
            if "tag" not in attrs:
                missing.append((role, obj.get_name()))

        walk(self.doc, visit)
        self.assertEqual(missing, [], f"nodes missing 'tag' attribute: {missing[:5]}")


class PasswordFieldTests(AccessibilityBridgeTestCase):
    """<input type=password> is a password text field, with its text masked.

    HTML-AAM gives a password input no ARIA role, so the tree's role is empty; the Qt bridge exposes it as
    EditableText with the passwordEdit state, which Qt's AT-SPI2 adaptor reports as the password text role —
    the role Gecko, WebKit and Blink give it. Its text is the mask the field renders (one U+25CF per character),
    never the password."""

    FIXTURE = "password.html"

    def test_password_field_has_the_password_text_role(self):
        password = wait_for_descendant_by_role(self.doc, Atspi.Role.PASSWORD_TEXT, name="Password")
        self.assertIsNotNone(password, "the password input isn't exposed with the password text role")
        # Control: the text input beside it is a plain text object (Qt's AT-SPI bridge reports EditableText as the
        # text role, QSpiAccessibleBridge::namesForRole).
        username = wait_for_descendant_by_role(self.doc, Atspi.Role.TEXT, name="Username")
        self.assertIsNotNone(username, "the text input isn't exposed as a text object")

    def test_password_field_text_is_masked(self):
        password = wait_for_descendant_by_role(self.doc, Atspi.Role.PASSWORD_TEXT, name="Password")
        self.assertIsNotNone(password, "the password input isn't exposed with the password text role")
        self.assertEqual(Atspi.Text.get_text(password, 0, -1), chr(0x25CF) * 7)
        # Control: the text input beside it reads its text in the clear.
        username = wait_for_descendant_by_role(self.doc, Atspi.Role.TEXT, name="Username")
        self.assertIsNotNone(username, "the text input isn't exposed as an entry")
        self.assertEqual(Atspi.Text.get_text(username, 0, -1), "ada")

    def test_unnamed_password_field_is_exposed(self):
        """A password input nothing names is in the tree too — Element::include_in_accessibility_tree() keeps it, since
        the empty role HTML-AAM gives it would otherwise leave it out — with the same role and the same masking."""
        unnamed = wait_for_descendant_by_role(self.doc, Atspi.Role.PASSWORD_TEXT, name="")
        self.assertIsNotNone(unnamed, "an unnamed password input isn't exposed with the password text role")
        self.assertEqual(Atspi.Text.get_text(unnamed, 0, -1), chr(0x25CF) * 6)
        # Control: the named one beside it is still its own node.
        named = wait_for_descendant_by_role(self.doc, Atspi.Role.PASSWORD_TEXT, name="Password")
        self.assertIsNotNone(named, "the named password input isn't exposed with the password text role")
        self.assertEqual(Atspi.Text.get_text(named, 0, -1), chr(0x25CF) * 7)


if __name__ == "__main__":
    unittest.main()
