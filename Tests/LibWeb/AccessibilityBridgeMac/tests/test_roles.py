"""Role-mapping invariants — what HTML/ARIA roles end up as on the macOS side.

The macOS wrapper's aria_role_to_ns_role + aria_role_to_ns_subrole pair (LadybirdAccessibilityElement.mm) is the
authority being defended here. Notable platform-specific behaviors:

* Headings are AXHeading with the level on AXValue (not a separate attribute).
* <li> maps to AXGroup — not a list-item role.
* All landmarks (nav/main/header/footer/aside/named-<section>) are AXGroup + AXLandmark* subrole. They share a common
  role and differ only in subrole + role-description.
* Document is AXWebArea with AXRoleDescription "HTML content"."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXRoleDescriptionAttribute  # noqa: E402
from ApplicationServices import kAXSubroleAttribute  # noqa: E402
from ApplicationServices import kAXTitleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import find_first_by_role  # noqa: E402
from harness import walk  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


def _find_by_subrole(root, subrole_name):
    """Return all descendants with the given AXSubrole. Convenience helper for landmark searches."""
    out = []

    def visit(obj, _depth):
        if _ax_attr(obj, kAXSubroleAttribute) == subrole_name:
            out.append(obj)

    walk(root, visit)
    return out


class BasicRoleTests(AccessibilityBridgeMacTestCase):
    """The straightforward role mappings: <h1>, <button>, <a>, <img>, <ul>, <li>."""

    FIXTURE = "roles.html"

    def test_3_headings_present_with_correct_levels(self):
        """<h1>..<h3> all map to AXHeading; AXValue carries the heading level (1, 2, 3)."""
        headings = find_all_by_role(self.web, "AXHeading")
        self.assertGreaterEqual(len(headings), 3, f"expected at least 3 headings, got {len(headings)}")

        by_level = {}
        for h in headings:
            level = _ax_attr(h, kAXValueAttribute)
            title = _ax_attr(h, kAXTitleAttribute)
            self.assertIsNotNone(level, f"heading {title!r} has no AXValue (no heading level)")
            by_level[level] = title

        self.assertEqual(by_level.get(1), "First heading")
        self.assertEqual(by_level.get(2), "Second heading")
        self.assertEqual(by_level.get(3), "Third heading")

    def test_button_role(self):
        """<button> maps to AXButton; the button name is on AXTitle."""
        btn = find_first_by_role(self.web, "AXButton")
        self.assertIsNotNone(btn, "expected an AXButton in the tree")
        self.assertEqual(_ax_attr(btn, kAXTitleAttribute), "A button")

    def test_link_role(self):
        """<a href> maps to AXLink. The fixture has 2 links (a nav link + a standalone link)."""
        links = find_all_by_role(self.web, "AXLink")
        names = [_ax_attr(link, kAXTitleAttribute) for link in links]
        self.assertIn("Standalone link", names, f"expected 'Standalone link' in {names}")
        self.assertIn("Nav link", names, f"expected 'Nav link' in {names}")

    def test_image_role(self):
        """<img alt> maps to AXImage; the alt text is the AXTitle."""
        img = find_first_by_role(self.web, "AXImage")
        self.assertIsNotNone(img, "expected an AXImage in the tree")
        self.assertEqual(_ax_attr(img, kAXTitleAttribute), "An image")

    def test_list_role(self):
        """<ul> maps to AXList."""
        lst = find_first_by_role(self.web, "AXList")
        self.assertIsNotNone(lst, "expected an AXList in the tree")

    def test_listitem_maps_to_axgroup(self):
        """<li> maps to AXGroup (NOT a list-item role)

        macOS NSAccessibility doesn't have a dedicated list-item role; the wrapper deliberately routes <li> through
        AXGroup."""
        ul = find_first_by_role(self.web, "AXList")
        self.assertIsNotNone(ul, "test prerequisite: AXList must exist")

        # The list's children should be AXGroup elements (the list items).
        from ApplicationServices import kAXChildrenAttribute

        children = _ax_attr(ul, kAXChildrenAttribute) or []
        self.assertGreater(len(children), 0, "AXList has no children. list-item leak?")
        roles = [_ax_attr(c, kAXRoleAttribute) for c in children]
        self.assertEqual(
            set(roles),
            {"AXGroup"},
            f"expected all <li> children to map to AXGroup, got {roles}",
        )


class LandmarkSubroleTests(AccessibilityBridgeMacTestCase):
    """All landmarks land at AXRole=AXGroup; what distinguishes them is AXSubrole.

    These tests defend the AXLandmark* subrole namespace from drift. VoiceOver's rotor uses these to populate the
    "Landmarks" submenu. A wrong subrole means the landmark vanishes from VO navigation."""

    FIXTURE = "roles.html"

    def test_landmark_navigation(self):
        """<nav> → AXGroup with AXSubrole=AXLandmarkNavigation."""
        navs = _find_by_subrole(self.web, "AXLandmarkNavigation")
        self.assertGreaterEqual(len(navs), 1, "expected at least one AXLandmarkNavigation")
        # Sanity: role is AXGroup
        self.assertEqual(_ax_attr(navs[0], kAXRoleAttribute), "AXGroup")

    def test_landmark_main(self):
        """<main> → AXGroup with AXSubrole=AXLandmarkMain."""
        mains = _find_by_subrole(self.web, "AXLandmarkMain")
        self.assertEqual(len(mains), 1, f"expected exactly one AXLandmarkMain, got {len(mains)}")

    def test_landmark_banner(self):
        """Top-level <header> → AXGroup with AXSubrole=AXLandmarkBanner.

        (HTMLElement.cpp's role-default logic only assigns 'banner' to a top-level <header> — one nested in
        article/aside/main/nav/section becomes 'sectionheader' instead, which has no landmark subrole.)"""
        banners = _find_by_subrole(self.web, "AXLandmarkBanner")
        self.assertGreaterEqual(len(banners), 1, "expected an AXLandmarkBanner from the top-level <header>")

    def test_landmark_contentinfo(self):
        """Top-level <footer> → AXGroup with AXSubrole=AXLandmarkContentInfo."""
        footers = _find_by_subrole(self.web, "AXLandmarkContentInfo")
        self.assertGreaterEqual(len(footers), 1, "expected an AXLandmarkContentInfo from the top-level <footer>")

    def test_landmark_complementary_for_aside(self):
        """<aside> (no sectioning ancestor that demotes it to generic) → AXLandmarkComplementary.

        roles.html nests <aside> inside <main>. <main> is *not* in HTMLElement.cpp's article/aside/nav/section demotion
        list. So the aside keeps its 'complementary' default role and gets the AXLandmarkComplementary subrole."""
        asides = _find_by_subrole(self.web, "AXLandmarkComplementary")
        self.assertGreaterEqual(len(asides), 1, "expected an AXLandmarkComplementary from <aside>")

    def test_landmark_region_for_named_section(self):
        """<section aria-label=...> → AXGroup with AXSubrole=AXLandmarkRegion.

        An *unnamed* <section> becomes role=generic (per HTMLElement.cpp) and is filtered out of the macOS tree — so
        this only fires for the named one."""
        regions = _find_by_subrole(self.web, "AXLandmarkRegion")
        self.assertGreaterEqual(len(regions), 1, "expected an AXLandmarkRegion from the named <section>")

    def test_unnamed_section_does_not_create_landmark(self):
        """The <section> with no aria-label or no inner heading is role=generic and ignored — must *not* produce a
        landmark. We confirm by counting AXLandmarkRegion: there's only 1 named <section>, so any second
        AXLandmarkRegion would mean an unnamed section snuck in."""
        regions = _find_by_subrole(self.web, "AXLandmarkRegion")
        self.assertLessEqual(
            len(regions),
            1,
            f"expected 0 or 1 AXLandmarkRegion (only the named <section> qualifies), got {len(regions)}",
        )


class RoleDescriptionTests(AccessibilityBridgeMacTestCase):
    """AXRoleDescription is what VoiceOver speaks aloud as the "type" of an element.

    The wrapper customizes a handful of these — landmarks plus heading and document — because
    NSAccessibilityRoleDescription doesn't recognize the AXLandmark* subrole strings. Without these custom descriptions,
    VoiceOver would speak "group" for every landmark."""

    FIXTURE = "roles.html"

    def test_document_role_description_is_html_content(self):
        """The AXWebArea reports AXRoleDescription="HTML content"."""
        self.assertEqual(_ax_attr(self.web, kAXRoleDescriptionAttribute), "HTML content")

    def test_heading_role_description_is_heading(self):
        """An AXHeading reports AXRoleDescription="heading" (the wrapper bypasses the OS default)."""
        h = find_first_by_role(self.web, "AXHeading")
        self.assertIsNotNone(h, "test prerequisite: an AXHeading must exist")
        self.assertEqual(_ax_attr(h, kAXRoleDescriptionAttribute), "heading")

    def test_landmark_role_descriptions(self):
        """Each AXLandmark* subrole has its own custom AXRoleDescription that VO can speak."""
        cases = [
            ("AXLandmarkNavigation", "navigation"),
            ("AXLandmarkMain", "main"),
            ("AXLandmarkBanner", "banner"),
            ("AXLandmarkContentInfo", "content information"),
            ("AXLandmarkComplementary", "complementary"),
            ("AXLandmarkRegion", "region"),
        ]
        for subrole, expected_desc in cases:
            with self.subTest(subrole=subrole):
                hits = _find_by_subrole(self.web, subrole)
                if not hits:
                    self.skipTest(f"no {subrole} in this fixture")
                actual = _ax_attr(hits[0], kAXRoleDescriptionAttribute)
                self.assertEqual(actual, expected_desc, f"AXRoleDescription for {subrole}")


if __name__ == "__main__":
    unittest.main()
