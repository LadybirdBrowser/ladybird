"""Historical regression tests — one test per fixed regression, named after the symptom.

Every time we fix a regression, a test goes here. These are duplicates of checks covered by the invariant-per-category
tests above, but named by the bug they catch — so when a test fails, the error message points at the user-visible bug
immediately, not just at some abstract invariant violation."""

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


class IKeyOnlyFirstCharRegressionTests(AccessibilityBridgeTestCase):
    """I-key problem.

    Symptom: pressing I in Orca to navigate to the next list item reads only the first character of each wrapping list
    item.

    Root cause: our LineBoundary override for textAtOffset applied visual-line splitting to leaf-like containers — so
    get_string_at_offset(li, 0, LINE) returned the first visual sub-line, rather than the full atomic text.

    Fix: restrict visual-line splitting to non-leaf-like containers."""

    FIXTURE = "listitems.html"

    def test_wrapping_listitem_not_truncated_to_first_char(self):
        items = find_all_by_role(self.doc, "list item")
        # The narrow ABC listitem wraps one char per line because of width: 20px.
        abc = next(
            (li for li in items if Atspi.Text.get_text(li, 0, Atspi.Text.get_character_count(li)) == "ABC"), None
        )
        self.assertIsNotNone(abc, "expected the narrow 'ABC' list item in the fixture")
        r = Atspi.Text.get_string_at_offset(abc, 0, Atspi.TextGranularity.LINE)
        self.assertEqual(
            r.content, "ABC", f"Orca would speak {r.content!r} instead of 'ABC' — I-key regression is back"
        )


class SayAllSkipsPostLinkProseRegressionTests(AccessibilityBridgeTestCase):
    """Say All used to jump link-to-link, silently dropping all non-link prose.

    Root cause: Qt's bridge exposes no AtkHypertext/AtkHyperlink, so Orca's _find_next_caret_in_order_internal, on
    exiting a link, failed its start+1 == end range-in-parent check and fell back to AXObject.get_next_sibling — which
    (because text-leaves are hidden) is always the next link.

    Fix: Orca-side hypertext fallback relying on the Nth U+FFFC marker in a paragraph corresponding to the Nth AT-SPI2
    exposed child.

    This test doesn't check the fix directly — it checks the underlying invariant the fix relies on."""

    FIXTURE = "paragraphs.html"

    def test_fffc_markers_align_with_exposed_children(self):
        ps = find_all_by_role(self.doc, "paragraph")
        for p in ps:
            count = Atspi.Text.get_character_count(p)
            text = Atspi.Text.get_text(p, 0, count) if count else ""
            fffc_positions = [i for i, c in enumerate(text) if c == "￼"]
            child_count = p.get_child_count()
            self.assertEqual(
                len(fffc_positions),
                child_count,
                f"paragraph with text {text!r} has {len(fffc_positions)} U+FFFC but "
                f"{child_count} exposed children — Orca's hypertext fallback will misalign",
            )


class TextLeafDuplicationRegressionTests(AccessibilityBridgeTestCase):
    """Flat review used to read link text twice ("See here for details here").

    Root cause: a paragraph exposed both its own Atspi.Text (with U+FFFC at link positions) *and* text-leaf children as
    AT-SPI2 children. Flat review built zones for both — duplicate coverage.

    Fix: collect_exposed_children hides text-leaf children for every container."""

    FIXTURE = "paragraphs.html"

    def test_paragraphs_expose_only_embedded_object_children(self):
        ps = find_all_by_role(self.doc, "paragraph")
        for p in ps:
            n = p.get_child_count()
            for i in range(n):
                child = p.get_child_at_index(i)
                self.assertNotEqual(
                    child.get_role_name(),
                    "text leaf",
                    "paragraph must not expose text-leaf children; flat review would duplicate-read",
                )


class WrappingTextYCollapseRegressionTests(AccessibilityBridgeTestCase):
    """Inline text in a wrapping paragraph used to report y=0 for every character.

    Root cause: LibWeb looked at the text node's own paintable for fragments, but inline text uses the containing
    block's PaintableWithLines.

    Fix: walk up to containing block, filter fragments by layout_node."""

    FIXTURE = "paragraphs.html"

    def test_wrapping_text_has_distinct_y_per_visual_line(self):
        ps = find_all_by_role(self.doc, "paragraph")
        wrapping = next(
            (
                p
                for p in ps
                if "deliberately long paragraph" in Atspi.Text.get_text(p, 0, Atspi.Text.get_character_count(p))
            ),
            None,
        )
        self.assertIsNotNone(wrapping)
        count = Atspi.Text.get_character_count(wrapping)
        ys = {
            Atspi.Text.get_character_extents(wrapping, i, Atspi.CoordType.SCREEN).y
            for i in range(count)
            if (
                Atspi.Text.get_character_extents(wrapping, i, Atspi.CoordType.SCREEN).width,
                Atspi.Text.get_character_extents(wrapping, i, Atspi.CoordType.SCREEN).height,
            )
            != (0, 0)
        }
        self.assertGreater(len(ys), 1, f"wrapping text characters all share y={ys} — the y-collapse regression is back")


class ListAdvertisesTextRegressionTests(AccessibilityBridgeTestCase):
    """<ul>/<ol> (role 'list') used to advertise Text, and Orca skipped the whole list in Say All.

    Root cause: all containers exposed Text by default. A list's text is [U+FFFC][U+FFFC][U+FFFC]… (one marker per
    listitem) — and without Hypertext Orca can't expand the markers, so it silently skipped listitems in Say All.

    Fix: list role explicitly excluded from TextInterface."""

    FIXTURE = "listitems.html"

    def test_list_does_not_advertise_text(self):
        from harness import find_first_by_role
        from harness import supports_interface

        lst = find_first_by_role(self.doc, "list")
        self.assertIsNotNone(lst)
        self.assertFalse(
            supports_interface(lst, "text"), "list must *not* advertise Text; otherwise Orca skips it during Say All"
        )


class ParagraphMappedToSectionRegressionTests(AccessibilityBridgeTestCase):
    """<p> used to map to role 'section', breaking Orca's Say All sentence extension.

    Root cause: no explicit paragraph role mapping; fell through to the section default. Orca's is_text_block_element
    requires ROLE_PARAGRAPH to stop walking across <p>.

    Fix: explicit paragraph role mapping."""

    FIXTURE = "roles.html"

    def test_paragraph_has_role_paragraph(self):
        ps = find_all_by_role(self.doc, "paragraph")
        self.assertGreater(len(ps), 0, "no paragraph-role accessibles found — regression to section mapping is back")


if __name__ == "__main__":
    unittest.main()
