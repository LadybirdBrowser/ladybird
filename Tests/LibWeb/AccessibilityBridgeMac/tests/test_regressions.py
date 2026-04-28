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
from ApplicationServices import AXUIElementSetAttributeValue  # noqa: E402
from ApplicationServices import kAXFocusedAttribute  # noqa: E402
from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import NotificationCollector  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import get_attribute_names  # noqa: E402
from harness import get_parameterized_attribute_value  # noqa: E402
from harness import wait_for  # noqa: E402
from harness import wait_for_descendant_by_role  # noqa: E402
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


def _static_text_values(root):
    """Every AXStaticText value under root, in tree order."""
    values = []

    def visit(obj, _depth):
        if _ax_attr(obj, kAXRoleAttribute) == "AXStaticText":
            value = _ax_attr(obj, kAXValueAttribute)
            if isinstance(value, str):
                values.append(value)

    walk(root, visit)
    return values


def _parse_cf_range(value):
    """Unwrap an AXValue CFRange into (location, length). PyObjC has no clean unwrapper for this opaque CFType, but
    its description is stable: "<AXValue 0x...> {value = location:2 length:1 type = kAXValueCFRangeType}"."""
    import re

    m = re.search(r"location:(\d+)\s+length:(\d+)", str(value))
    if m is None:
        return None
    return int(m.group(1)), int(m.group(2))


class EndTextMarkerOffsetIsInCodePointsTests(AccessibilityBridgeMacTestCase):
    """Symptom: on a page whose text ends with a non-ASCII character, VoiceOver's cursor could be placed past the end
    of the document, and reading backward from the end announced phantom characters before reaching the last real one.

    Root cause: AXEndTextMarker put the last text leaf's UTF-8 byte length in the marker's offset, while every other
    marker the wrapper hands out (AXNextTextMarkerForTextMarker, AXTextMarkerRangeForUIElement) counts code points. A
    leaf ending in an emoji (4 bytes, 1 code point) so got an end marker 3 positions past its last character.

    Fix: build the end marker from codePointCount(), like the other markers."""

    FIXTURE = "nonbmp_text.html"

    def test_steps_back_from_end_marker_to_its_leaf_start_equal_the_leaf_code_point_count(self):
        """Walking AXPreviousTextMarkerForTextMarker from AXEndTextMarker stays inside the last leaf for exactly as
        many steps as that leaf has code points: the end marker sits at offset N, each step moves to N-1, and the step
        from offset 0 crosses into the previous leaf. A byte-based end offset takes extra steps for each multi-byte
        character in the leaf."""
        end = _ax_attr(self.web, "AXEndTextMarker")
        self.assertIsNotNone(end, "AXWebArea must expose AXEndTextMarker")
        end_leaf = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", end)
        self.assertIsNotNone(end_leaf, "AXUIElementForTextMarker(AXEndTextMarker) returned nil")
        self.assertEqual(_ax_attr(end_leaf, kAXRoleAttribute), "AXStaticText")
        leaf_value = _ax_attr(end_leaf, kAXValueAttribute)
        self.assertIsInstance(leaf_value, str)
        self.assertTrue(
            any(ord(ch) > 0xFFFF for ch in leaf_value),
            f"fixture's last leaf must end in a non-BMP character to make the units observable: {leaf_value!r}",
        )

        steps = 0
        marker = end
        # Bound the walk generously above the byte length so a runaway loop fails the test rather than hanging it.
        limit = 4 * len(leaf_value) + 16
        while steps <= limit:
            prev = get_parameterized_attribute_value(self.web, "AXPreviousTextMarkerForTextMarker", marker)
            if prev is None:
                break
            leaf = get_parameterized_attribute_value(self.web, "AXUIElementForTextMarker", prev)
            if leaf != end_leaf:
                break
            steps += 1
            marker = prev

        self.assertEqual(
            steps,
            len(leaf_value),
            f"AXEndTextMarker is {steps} steps from the start of its leaf, but the leaf has {len(leaf_value)} code "
            f"points ({len(leaf_value.encode('utf-8'))} UTF-8 bytes): {leaf_value!r}",
        )


class LengthForTextMarkerRangeIsInCodePointsTests(AccessibilityBridgeMacTestCase):
    """Symptom: for text containing characters outside the Basic Multilingual Plane, AXLengthForTextMarkerRange
    disagreed with the number of AXNextTextMarkerForTextMarker steps that cover the same range, so VoiceOver's
    character-by-character navigation and its range bookkeeping fell out of step by one per such character.

    Root cause: the wrapper answered AXLengthForTextMarkerRange with the NSString's length, which counts UTF-16 code
    units, while markers step and slice by code point.

    Fix: count the code points appended to the range's string, and report that."""

    FIXTURE = "nonbmp_text.html"

    def test_length_equals_the_code_point_count_of_the_range_string(self):
        leaves = find_all_by_role(self.web, "AXStaticText")
        checked = 0
        for leaf in leaves:
            leaf_value = _ax_attr(leaf, kAXValueAttribute)
            if not isinstance(leaf_value, str) or not any(ord(ch) > 0xFFFF for ch in leaf_value):
                continue
            marker_range = get_parameterized_attribute_value(leaf, "AXTextMarkerRangeForUIElement", leaf)
            self.assertIsNotNone(marker_range, f"AXTextMarkerRangeForUIElement returned nil for {leaf_value!r}")
            text = get_parameterized_attribute_value(self.web, "AXStringForTextMarkerRange", marker_range)
            length = get_parameterized_attribute_value(self.web, "AXLengthForTextMarkerRange", marker_range)
            self.assertEqual(text, leaf_value)
            self.assertEqual(
                length,
                len(text),
                f"AXLengthForTextMarkerRange={length!r} but the range's string has {len(text)} code points "
                f"({len(text.encode('utf-16-le')) // 2} UTF-16 code units): {text!r}",
            )
            checked += 1
        self.assertGreater(checked, 0, "fixture must contain at least one text leaf with a non-BMP character")


class ColspanCellColumnIndexTests(AccessibilityBridgeMacTestCase):
    """Symptom: in a table whose row starts with a cell spanning two columns, VoiceOver announced the next cell as
    column 2 rather than column 3, and its AXColumnIndexRange overlapped the spanning cell's.

    Root cause: the wrapper computed a cell's column index by counting its preceding siblings in the row, one column
    each, ignoring their colspan.

    Fix: use the colspan-aware column index WebContent already serializes for every table cell, and keep the sibling
    count only as a fallback for a cell that comes without one."""

    FIXTURE = "table_colspan.html"

    def _cell_with_text(self, text):
        cells = find_all_by_role(self.web, "AXCell")
        self.assertGreater(len(cells), 0, "expected AXCell elements in table_colspan.html")
        for cell in cells:
            if text in _static_text_values(cell):
                return cell
        self.fail(f"no AXCell contains the text {text!r}")

    def test_cell_after_a_two_column_cell_starts_at_column_two(self):
        after = self._cell_with_text("After the wide cell")
        rng = _parse_cf_range(_ax_attr(after, "AXColumnIndexRange"))
        self.assertIsNotNone(rng, "AXColumnIndexRange must be a CFRange AXValue")
        self.assertEqual(rng, (2, 1), f"cell after a colspan=2 cell should occupy column 2 alone, got {rng}")

    def test_two_column_cell_spans_columns_zero_and_one(self):
        wide = self._cell_with_text("Wide cell")
        rng = _parse_cf_range(_ax_attr(wide, "AXColumnIndexRange"))
        self.assertIsNotNone(rng, "AXColumnIndexRange must be a CFRange AXValue")
        self.assertEqual(rng, (0, 2), f"a colspan=2 cell in the first column should report (0, 2), got {rng}")

    def test_plain_row_cells_count_up_from_zero(self):
        for text, expected in (("One", 0), ("Two", 1), ("Three", 2)):
            rng = _parse_cf_range(_ax_attr(self._cell_with_text(text), "AXColumnIndexRange"))
            self.assertEqual(rng, (expected, 1), f"cell {text!r} should be at column {expected}, got {rng}")


def _wait_for_a_mutation_push(ctx, timeout=10.0, pump=None):
    """Waits until the page's ticking paragraph reads something other than its initial "Tick 0" in a fresh lookup of
    the AXWebArea, which proves at least one post-load tree update reached the wrapper. "pump" is called on every poll
    so a caller with an AXObserver can keep its run loop turning meanwhile."""

    def ticked():
        if pump is not None:
            pump()
        web = ctx.find_web_area()
        if web is None:
            return False
        return any(value.startswith("Tick ") and value != "Tick 0" for value in _static_text_values(web))

    return wait_for(ticked, timeout=timeout, interval=0.2, description="a mutation-driven tree update")


class TreeUpdateDoesNotRefocusDocumentTests(AccessibilityBridgeMacTestCase):
    """Symptom: while VoiceOver was reading a page, or the user was typing into a form field on it, any DOM change made
    VoiceOver jump back to the top of the document and re-announce the page as newly loaded.

    Root cause: the tree-received handler treated every tree the same way, whether it answered the load-time request or
    a push after a DOM mutation. Each one took keyboard first-responder status, posted AXFocusedUIElementChanged on
    the AXWebArea, posted AXLoadComplete, and refreshed AppKit's focus cache.

    Fix: gate that block on a per-page flag that the load-finish path resets, so only the first tree for a page takes
    focus and announces the load; later pushes post only AXLayoutChanged."""

    FIXTURE = "mutations.html"

    def test_mutation_pushes_post_no_load_complete_and_no_root_focus_change(self):
        collector = NotificationCollector(
            self.ctx.pid, self.app, ["AXLoadComplete", "AXFocusedUIElementChanged", "AXLayoutChanged"]
        )
        try:
            # Let anything still in flight from the initial load land before the experiment starts.
            collector.collect(1.0)
            collector.clear()

            link = wait_for_descendant_by_role(self.web, "AXLink")
            self.assertIsNotNone(link, "mutations.html must expose an AXLink")
            err = AXUIElementSetAttributeValue(link, kAXFocusedAttribute, True)
            self.assertEqual(err, 0, "setting AXFocused on the link failed")
            self.assertTrue(
                _wait_for_a_mutation_push(self.ctx, pump=lambda: collector.collect(0.1)),
                "no mutation-driven tree update arrived within 10s",
            )
            # Several more pushes: the page mutates every 300 ms and WebContent debounces by 200 ms.
            collector.collect(3.0)
        finally:
            collector.close()

        names = collector.names()
        # Control: the pushes are observable at all. Every tree update posts AXLayoutChanged, gated or not.
        self.assertIn("AXLayoutChanged", names, f"no AXLayoutChanged seen; notifications received: {names}")
        self.assertNotIn(
            "AXLoadComplete",
            names,
            "a DOM-mutation tree update re-announced the page as loaded (AXLoadComplete); VoiceOver treats that as a "
            f"new page and jumps to the top. Notifications received: {names}",
        )
        for elem in collector.elements_for("AXFocusedUIElementChanged"):
            self.assertNotEqual(
                _ax_attr(elem, kAXRoleAttribute),
                "AXWebArea",
                "a DOM-mutation tree update posted AXFocusedUIElementChanged on the AXWebArea, pulling VoiceOver's "
                f"cursor back to the document root. Notifications received: {names}",
            )


if __name__ == "__main__":
    unittest.main()
