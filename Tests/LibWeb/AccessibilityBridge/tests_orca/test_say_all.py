"""Layer-2 tests for Orca Say All (KP+ / Orca+Semicolon) on Ladybird.

Guards against regressing the problem fixed by the Orca-side hypertext-fallback patch: previously, our code made Say All
jump link-to-link across a paragraph — silently dropping link text *and* the post-link prose. The fixed code restores
Say All to read the whole paragraph in visual order: pre-link text, link text (announced as a link), post-link text."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402
from harness import LadybirdOrcaTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402


def _full_text(obj):
    try:
        count = Atspi.Text.get_character_count(obj)
    except Exception:
        return ""
    return Atspi.Text.get_text(obj, 0, count) if count else ""


class SayAllSentenceContentsTests(LadybirdOrcaTestCase):
    """What Orca's sentence-extension code produces for a paragraph with inline links.

    Rather than drive Orca's full Say All iterator (which pulls in speech dispatcher and async machinery), we exercise
    the one function inside that drives the behavior: web.Utilities.get_sentence_contents_at_offset. If it returns the
    right tuples, Say All will speak them correctly."""

    FIXTURE = "paragraphs.html"

    def test_paragraph_with_one_link_contents_include_pre_and_post_prose(self):
        """Ensure Say All doesn't entirely drop post-link prose.

        A fix in our code makes _find_next_caret_in_order_internal, on exiting a link, ascend back to the paragraph at
        offset fffc+1 — where pre-link and post-link prose are stitched into the sentence contents."""
        ps = find_all_by_role(self.doc, "paragraph")
        p = next((p for p in ps if _full_text(p).startswith("A paragraph with")), None)
        self.assertIsNotNone(p, "fixture missing one-link paragraph")

        utils = self.orca.script.utilities
        contents = utils.get_sentence_contents_at_offset(p, 0)
        # Contents is a list of (obj, start, end, string) tuples in visual order.
        self.assertGreater(len(contents), 1, f"expected multiple tuples in sentence contents; got {contents!r}")

        strings = [text for (_o, _s, _e, text) in contents]
        joined = " ".join(strings)
        self.assertIn("A paragraph with", joined, f"pre-link prose missing from sentence contents: {strings!r}")
        self.assertIn("link embedded", joined, f"post-link prose missing from sentence contents: {strings!r}")

    def test_paragraph_with_two_links_reads_in_dom_order(self):
        """Two inline links: Orca must walk pre, link1, between, link2, after — in order."""
        ps = find_all_by_role(self.doc, "paragraph")
        p = next((p for p in ps if _full_text(p).startswith("Paragraph with")), None)
        self.assertIsNotNone(p, "fixture missing two-link paragraph")

        utils = self.orca.script.utilities
        contents = utils.get_sentence_contents_at_offset(p, 0)
        strings = [text for (_o, _s, _e, text) in contents]
        joined = " ".join(strings)

        self.assertIn("Paragraph with", joined)
        self.assertIn("and", joined, f"between-link prose missing: {strings!r}")
        self.assertIn("links.", joined, f"post-link prose missing: {strings!r}")

        # Also confirm Orca sees a link object for each of the two links — so they'll be announced with the 'link' role
        # in speech.
        link_objs = [obj for (obj, _s, _e, _t) in contents if obj.get_role_name() == "link"]
        self.assertGreaterEqual(
            len(link_objs), 2, f"expected two link tuples; got {[o.get_role_name() for (o, _, _, _) in contents]}"
        )


if __name__ == "__main__":
    unittest.main()
