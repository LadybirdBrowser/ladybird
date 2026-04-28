"""Regression tests — one per fixed bug, named after the symptom.

When you fix a bug in this area of the code, add a regression test (class) here that would have caught it. Use the
docstring for the class to describe the symptom + root cause + fix. Cite commit hashes, if you have them."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import AXUIElementCopyAttributeValue  # noqa: E402
from ApplicationServices import AXUIElementIsAttributeSettable  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import get_attribute_names  # noqa: E402
from harness import get_parameterized_attribute_value  # noqa: E402
from harness import walk  # noqa: E402


def _ax_attr(elem, key):
    if elem is None:
        return None
    err, value = AXUIElementCopyAttributeValue(elem, key, None)
    return None if err != 0 else value


class SelectedTextMarkerRangeLoopRegressionTests(AccessibilityBridgeMacTestCase):
    """Symptom: with VoiceOver auto-read on a multi-paragraph page, VO reads the first paragraph, then immediately
    re-reads it again and again — never advancing.

    Root cause: Our code tried adding an AXSelectedTextMarkerRange getter on the AXWebArea that always returned a fixed
    zero-width range pinned to the start of the first text leaf — with no setter to accept VoiceOver's cursor writes.
    VoiceOver's continuous-read consults that attribute after each chunk to confirm cursor advancement, gets back the
    same start anchor every time, concludes either "the page reset" or "the cursor never advanced", and then re-reads
    the current chunk — forever.

    Fix: Don't advertise the attribute at all unless we actually track cursor state and accept writes. So, test: if the
    attribute *is* advertised, it must be settable.  A non-settable, fixed-value advertisement is the failure mode."""

    FIXTURE = "paragraphs.html"

    def test_axselectedtextmarkerrange_is_settable_if_advertised(self):
        """If AXSelectedTextMarkerRange is on the attribute list, it must be settable (have a setter that accepts
        VoiceOver's cursor writes)."""
        attr_names = get_attribute_names(self.web)
        if "AXSelectedTextMarkerRange" not in attr_names:
            # OK — not advertised at all. That's the V1 fix state.
            return
        err, settable = AXUIElementIsAttributeSettable(self.web, "AXSelectedTextMarkerRange", None)
        self.assertEqual(err, 0, "AXUIElementIsAttributeSettable returned non-zero err")
        self.assertTrue(
            settable,
            "AXSelectedTextMarkerRange is advertised on the AXWebArea but is *not* settable. "
            "VoiceOver's continuous-read writes to this attribute to advance its cursor; without "
            "a setter, every read returns the same fixed value, and VO+A enters an infinite "
            "re-read loop on the first paragraph.",
        )

    def test_axvisiblecharacterrange_is_not_a_lie(self):
        """If AXVisibleCharacterRange is advertised on the AXWebArea, it must report a length consistent with the actual
        text content — not a lie like (0, 1) that claims one character of the entire document is visible."""
        attr_names = get_attribute_names(self.web)
        if "AXVisibleCharacterRange" not in attr_names:
            return
        rng = _ax_attr(self.web, "AXVisibleCharacterRange")
        # The CFRange comes back as an opaque AXValue. Its repr looks like:
        #   <AXValue> {value = location:0 length:NNN type = kAXValueCFRangeType}
        # We don't have a clean unwrap helper here — so do a string-pattern check.
        rng_str = str(rng)
        self.assertIn("kAXValueCFRangeType", rng_str)
        # paragraphs.html has well over 100 characters of body text; a length of 0 or 1 is a lie. Use a stringly-typed
        # check: the value should not be 'length:0' or 'length:1'.
        self.assertNotIn("length:0 ", rng_str, f"AXVisibleCharacterRange reports length 0: {rng_str}")
        self.assertNotIn("length:1 ", rng_str, f"AXVisibleCharacterRange reports length 1: {rng_str}")


class AccessibilityFocusedUIElementStableTests(AccessibilityBridgeMacTestCase):
    """Symptom: VoiceOver's accessibilityFocusedUIElement appears to bounce or reset on every read after page load —
    causing AT clients to perceive repeated page-load events.

    Defense: read the application's accessibilityFocusedUIElement repeatedly — and assert it stays stable across calls
    (no value changes when nothing the test does could cause focus to move)."""

    FIXTURE = "paragraphs.html"

    def test_focused_ui_element_is_stable_across_reads(self):
        """Reading the application's AXFocusedUIElement five times in quick succession should return the same element
        each time. If it bounces, downstream VO behavior is unpredictable."""
        from ApplicationServices import kAXFocusedUIElementAttribute

        first = _ax_attr(self.app, kAXFocusedUIElementAttribute)
        # If there's no focused element at all, the page likely has no focusable elements; that's OK and not a
        # regression.
        if first is None:
            return
        for _ in range(4):
            now = _ax_attr(self.app, kAXFocusedUIElementAttribute)
            # PyObjC AXUIElement equality is by identity — but two AXUIElements referring to the same backing object
            # compare equal. So plain == is the right check.
            self.assertEqual(
                now,
                first,
                "accessibilityFocusedUIElement bounces across reads — VoiceOver will see this as "
                "constant focus changes and may misbehave",
            )


class ParagraphTraversalAdvancesTests(AccessibilityBridgeMacTestCase):
    """Symptom-level defense: VO+A on a multi-paragraph page must advance forward through paragraphs rather than looping
    on the first one. This test simulates VO's forward navigation by calling AXUIElementsForSearchPredicate from each
    paragraph-text leaf and asserting the search advances to a different element."""

    FIXTURE = "paragraphs.html"

    def test_search_predicate_forward_advances(self):
        """Find each AXStaticText under the AXWebArea, ask for "next" via the search predicate, and assert that the
        result is *not* the same element."""
        seen_advances = 0
        loops_detected = []

        text_leaves = []

        def collect_text(obj, _depth):
            if _ax_attr(obj, kAXRoleAttribute) == "AXStaticText":
                text_leaves.append(obj)

        walk(self.web, collect_text)
        self.assertGreater(len(text_leaves), 1, "expected multiple AXStaticText leaves in paragraphs.html")

        forward_pred = {
            "AXDirection": "AXDirectionNext",
            "AXResultsLimit": 1,
            "AXImmediateDescendantsOnly": False,
        }

        for leaf in text_leaves[:5]:  # cap at 5 — don't hammer
            forward_pred_with_start = dict(forward_pred)
            forward_pred_with_start["AXStartElement"] = leaf
            result = get_parameterized_attribute_value(
                self.web, "AXUIElementsForSearchPredicate", forward_pred_with_start
            )
            if not result:
                continue  # last leaf: no next; that's fine
            next_element = result[0]
            value_before = _ax_attr(leaf, kAXValueAttribute)
            value_after = _ax_attr(next_element, kAXValueAttribute)
            if next_element == leaf:
                loops_detected.append(repr(value_before))
            elif value_before != value_after:
                seen_advances += 1
            else:
                # Same value but different element — fine, two leaves can have the same text.
                seen_advances += 1

        self.assertEqual(loops_detected, [], f"search predicate returned starting element as 'next': {loops_detected}")
        self.assertGreater(seen_advances, 0, "search predicate never advanced past any leaf")


class TextLeafFocusActionDoesNotPromoteInlineAncestorTests(AccessibilityBridgeMacTestCase):
    """Symptom: VoiceOver auto-read pauses on each abbr (and some other element cases). VoiceOver reads the abbr's text,
    then interrupts itself to announce, "Foo bar baz, group, inside heading level 1" — where "Foo bar baz” is the abbr's
    title attribute exposed as a "selectable group" name.

    Root cause: When VO set focus on an AXStaticText leaf via accessibilitySetValue:NSAccessibilityFocusedAttribute=YES,
    our perform_accessibility_action handler walked up to the leaf's DOM parent (e.g., an abbr element), force-added
    tabindex=-1 on it, and called run_focusing_steps. The abbr then got DOM focus — so include_in_accessibility_tree
    returned true, and the abbr appeared as a separate AX-tree node. Its empty role mapped to NSAccessibilityGroupRole;
    its accessible name picked up the title attribute. So VoiceOver received the focus-changed notification and then
    unexpectedly announced the abbr content as a "selectable group".

    Fix: In the focus-action branch of perform_accessibility_action, return early when the original target is a
    non-element (text node) and we walked up to find an element."""

    FIXTURE = "abbreviations.html"

    def test_focus_action_on_abbr_text_leaf_does_not_create_inline_group(self):
        """Find the AXStaticText whose value is "CSS" (the abbr text), snapshot its parent-chain roles, send the focus
        action on it, wait for the debounced tree refresh, and verify no new AXGroup with the abbr's title appeared in
        the chain."""
        import time

        from ApplicationServices import AXUIElementSetAttributeValue
        from ApplicationServices import kAXFocusedAttribute
        from ApplicationServices import kAXParentAttribute
        from ApplicationServices import kAXTitleAttribute

        # The fixture has <abbr title="Cascading Style Sheets">CSS</abbr> inside the H1. Find that specific text leaf
        # rather than just any AXStaticText.
        abbr_leaf = None

        def find_abbr_text(obj, _depth):
            nonlocal abbr_leaf
            if (
                abbr_leaf is None
                and _ax_attr(obj, kAXRoleAttribute) == "AXStaticText"
                and _ax_attr(obj, kAXValueAttribute) == "CSS"
            ):
                abbr_leaf = obj

        walk(self.web, find_abbr_text)
        self.assertIsNotNone(abbr_leaf, "abbreviations.html should expose AXStaticText with value 'CSS'")

        def parent_chain(elem, n=6):
            chain = []
            cur = elem
            for _ in range(n):
                if cur is None:
                    break
                chain.append((_ax_attr(cur, kAXRoleAttribute), _ax_attr(cur, kAXTitleAttribute)))
                cur = _ax_attr(cur, kAXParentAttribute)
            return chain

        # Snapshot the chain before the focus action (used in failure messages for diagnostics).
        before_chain = parent_chain(abbr_leaf)  # noqa: F841

        err = AXUIElementSetAttributeValue(abbr_leaf, kAXFocusedAttribute, True)
        self.assertEqual(err, 0)
        # WebContent debounces accessibility tree updates ~200 ms; allow extra slack.
        time.sleep(1.5)

        # Re-acquire the leaf — element handles can become stale across tree refreshes.
        new_leaf = None

        def find_abbr_text_after(obj, _depth):
            nonlocal new_leaf
            if (
                new_leaf is None
                and _ax_attr(obj, kAXRoleAttribute) == "AXStaticText"
                and _ax_attr(obj, kAXValueAttribute) == "CSS"
            ):
                new_leaf = obj

        walk(self.web, find_abbr_text_after)
        self.assertIsNotNone(new_leaf, "AXStaticText 'CSS' disappeared after focus action")

        after_chain = parent_chain(new_leaf)

        # Specifically: no new AXGroup should appear in the chain whose title is the abbr's title attribute.
        for role, title in after_chain:
            self.assertFalse(
                role == "AXGroup" and title == "Cascading Style Sheets",
                f"focus action on the abbr text leaf created a new AXGroup with title='Cascading "
                f"Style Sheets' (the abbr's title attribute). Parent chain after: {after_chain}. "
                "This is the exact failure mode behind VoiceOver interrupting auto-read on each abbr.",
            )


if __name__ == "__main__":
    unittest.main()
