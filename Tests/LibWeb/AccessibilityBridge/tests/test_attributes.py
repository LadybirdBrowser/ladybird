"""Object-attribute invariants: "tag", "xml-roles", "level".

These are what Orca's landmark navigation and heading navigation depend on."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi  # noqa: E402

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import AccessibilityBridgeTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import get_attributes_dict  # noqa: E402
from harness import walk  # noqa: E402


class TagAttributeTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def test_every_role_exposes_tag(self):
        """Every exposed node has a "tag" object attribute."""
        missing = []

        def visit(obj, _depth):
            try:
                role = obj.get_role_name()
                attrs = get_attributes_dict(obj)
                if role in ("document web", "filler"):
                    return
                if "tag" not in attrs:
                    missing.append((role, obj.get_name()))
            except Exception:
                pass

        walk(self.doc, visit)
        self.assertEqual(missing, [], f"nodes missing 'tag': {missing[:5]}")

    def test_paragraph_tag_is_p(self):
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0)
        for p in ps:
            self.assertEqual(get_attributes_dict(p).get("tag"), "p")

    def test_button_tag_is_button(self):
        btn = find_first_by_role(self.doc, Atspi.Role.PUSH_BUTTON)
        self.assertIsNotNone(btn)
        self.assertEqual(get_attributes_dict(btn).get("tag"), "button")

    def test_heading_tags_are_hN(self):
        for h in find_all_by_role(self.doc, "heading"):
            attrs = get_attributes_dict(h)
            level = attrs.get("level")
            self.assertIsNotNone(level)
            self.assertEqual(attrs.get("tag"), f"h{level}")

    def test_list_tag_is_ul(self):
        lst = find_first_by_role(self.doc, "list")
        self.assertIsNotNone(lst)
        self.assertEqual(get_attributes_dict(lst).get("tag"), "ul")

    def test_listitem_tag_is_li(self):
        li = find_first_by_role(self.doc, "list item")
        self.assertIsNotNone(li)
        self.assertEqual(get_attributes_dict(li).get("tag"), "li")

    def test_link_tag_is_a(self):
        link = find_first_by_role(self.doc, "link")
        self.assertIsNotNone(link)
        self.assertEqual(get_attributes_dict(link).get("tag"), "a")

    def test_image_tag_is_img(self):
        img = find_first_by_role(self.doc, "image")
        self.assertIsNotNone(img)
        self.assertEqual(get_attributes_dict(img).get("tag"), "img")


class XmlRolesAttributeTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def _all_xml_roles(self):
        out = {}

        def visit(obj, _depth):
            try:
                attrs = get_attributes_dict(obj)
                xr = attrs.get("xml-roles")
                if xr:
                    out.setdefault(xr, []).append(obj.get_name())
            except Exception:
                pass

        walk(self.doc, visit)
        return out

    def test_nav_xml_role_navigation(self):
        """<nav> → xml-roles:navigation."""
        self.assertIn("navigation", self._all_xml_roles())

    def test_main_xml_role_main(self):
        self.assertIn("main", self._all_xml_roles())

    def test_top_level_header_xml_role_banner(self):
        self.assertIn("banner", self._all_xml_roles())

    def test_top_level_footer_xml_role_contentinfo(self):
        self.assertIn("contentinfo", self._all_xml_roles())

    def test_article_xml_role_article(self):
        self.assertIn("article", self._all_xml_roles())

    def test_aside_xml_role_complementary(self):
        self.assertIn("complementary", self._all_xml_roles())


class LevelAttributeTests(AccessibilityBridgeTestCase):
    FIXTURE = "roles.html"

    def test_headings_have_correct_level(self):
        """<h1>..<hN> expose "level" attribute 1..N."""
        hs = find_all_by_role(self.doc, "heading")
        levels = {get_attributes_dict(h).get("level"): h.get_name() for h in hs}
        self.assertEqual(levels.get("1"), "First heading")
        self.assertEqual(levels.get("2"), "Second heading")
        self.assertEqual(levels.get("3"), "Third heading")

    def test_non_heading_has_no_level(self):
        """Non-heading elements have no "level" attribute."""
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0)
        for p in ps:
            self.assertNotIn("level", get_attributes_dict(p))


if __name__ == "__main__":
    unittest.main()
