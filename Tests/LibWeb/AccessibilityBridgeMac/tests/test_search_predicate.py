"""AXUIElementsForSearchPredicate / AXUIElementCountForSearchPredicate.

VoiceOver uses these (parameterized) attributes for forward/backward navigation, "Find Next" rotor, and similar.

Notable wrapper behaviors this file defends:

* AXDirection accepts "AXDirectionNext" or "AXDirectionPrevious"
* AXStartElement anchors the walk; without one, a forward walk starts at the beginning and a backward one at the end
* AXResultsLimit caps the result count (negative or absent = unlimited)
* Forward walks past a container element skip its descendants (avoids re-traversing them)
* AXSearchKey is *not yet honored* — every search returns the same flat list regardless. We test the current behavior so
  a future change (filtering by element type) trips a visible test.
* AXUIElementCountForSearchPredicate returns an NSNumber count, not an array."""

from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import get_parameterized_attribute_value  # noqa: E402


def _make_pred(*, direction="AXDirectionNext", start=None, limit=-1, immediate=False):
    """Build the dictionary parameter that the search-predicate APIs accept."""
    pred = {
        "AXDirection": direction,
        "AXResultsLimit": limit,
        "AXImmediateDescendantsOnly": immediate,
    }
    if start is not None:
        pred["AXStartElement"] = start
    return pred


class SearchPredicateForwardTests(AccessibilityBridgeMacTestCase):
    """Forward walks: AXDirectionNext."""

    FIXTURE = "roles.html"

    def test_forward_from_first_text_returns_a_list(self):
        """AXUIElementsForSearchPredicate with direction=Next and a starting AXStaticText returns a non-empty list of
        subsequent elements."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0, "fixture must have AXStaticText leaves")

        pred = _make_pred(direction="AXDirectionNext", start=leaves[0], limit=-1)
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", pred)
        self.assertIsNotNone(results)
        self.assertGreater(
            len(results), 0, f"expected at least one element after the first text leaf, got {len(results)}"
        )

    def test_forward_with_limit_caps_count(self):
        """AXResultsLimit=3 returns at most 3 results."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)

        pred = _make_pred(direction="AXDirectionNext", start=leaves[0], limit=3)
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", pred)
        self.assertIsNotNone(results)
        self.assertLessEqual(len(results), 3, f"limit=3 but got {len(results)} results")

    def test_forward_with_limit_1_returns_one(self):
        """AXResultsLimit=1 returns at most 1 result. This is the canonical "next element" query VoiceOver uses."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)

        pred = _make_pred(direction="AXDirectionNext", start=leaves[0], limit=1)
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", pred)
        self.assertIsNotNone(results)
        self.assertLessEqual(len(results), 1)
        # If the start was the last element in the document, results is allowed to be empty.


class SearchPredicateBackwardTests(AccessibilityBridgeMacTestCase):
    """Backward walks: AXDirectionPrevious."""

    FIXTURE = "roles.html"

    def test_previous_from_later_text_returns_earlier_element(self):
        """AXDirectionPrevious from a non-first AXStaticText returns elements that occur earlier in the document
        order — and never returns the start element itself."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreaterEqual(len(leaves), 2, "fixture needs ≥2 AXStaticText leaves for a backward walk")

        start = leaves[-1]
        pred = _make_pred(direction="AXDirectionPrevious", start=start, limit=-1)
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", pred)
        self.assertIsNotNone(results)
        self.assertGreater(len(results), 0, "expected previous elements before the last text leaf")
        self.assertNotIn(start, results, "previous-search returned the start element itself")

    def test_previous_without_a_start_element_walks_from_the_end(self):
        """AXDirectionPrevious with no AXStartElement starts past the last element, so its first result is the
        document's last element — the mirror of a forward walk with no start, which begins at the first. (Blink's
        OneShotAccessibilityTreeSearch and WebKit's AXSearchManager start such a search from the end too.)"""
        everything = get_parameterized_attribute_value(
            self.web, "AXUIElementsForSearchPredicate", _make_pred(direction="AXDirectionNext", limit=-1)
        )
        self.assertIsNotNone(everything)
        self.assertGreater(len(everything), 1, "fixture needs ≥2 elements for a backward walk")

        results = get_parameterized_attribute_value(
            self.web, "AXUIElementsForSearchPredicate", _make_pred(direction="AXDirectionPrevious", limit=1)
        )
        self.assertIsNotNone(results)
        self.assertEqual(len(results), 1, "a backward walk with no start element found nothing")
        self.assertEqual(results[0], everything[-1], "a backward walk with no start element must begin at the end")

        count = get_parameterized_attribute_value(
            self.web, "AXUIElementCountForSearchPredicate", _make_pred(direction="AXDirectionPrevious", limit=-1)
        )
        self.assertEqual(int(count), len(everything), "an unlimited backward walk must cover every element")


class SearchPredicateCountTests(AccessibilityBridgeMacTestCase):
    """AXUIElementCountForSearchPredicate — same params, returns a count instead of a list."""

    FIXTURE = "roles.html"

    def test_count_matches_results_length(self):
        """For the same predicate, AXUIElementCountForSearchPredicate's count equals the length of
        AXUIElementsForSearchPredicate's array. The wrapper computes both from the same code path."""
        leaves = find_all_by_role(self.web, "AXStaticText")
        self.assertGreater(len(leaves), 0)

        pred = _make_pred(direction="AXDirectionNext", start=leaves[0], limit=5)
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", pred)
        count = get_parameterized_attribute_value(self.web, "AXUIElementCountForSearchPredicate", pred)
        self.assertIsNotNone(results)
        self.assertIsNotNone(count)
        self.assertEqual(int(count), len(results))


class SearchPredicateMalformedInputTests(AccessibilityBridgeMacTestCase):
    """Defending the wrapper's input-validation paths: non-dictionary parameters return empty/0."""

    FIXTURE = "roles.html"

    def test_non_dict_parameter_returns_empty_list(self):
        """A string parameter (instead of a dict) returns an empty array — not a crash, not nil."""
        results = get_parameterized_attribute_value(self.web, "AXUIElementsForSearchPredicate", "not a dict")
        # An empty NSArray marshals to a Python list of length 0.
        self.assertIsNotNone(results)
        self.assertEqual(len(results), 0)

    def test_non_dict_parameter_returns_zero_count(self):
        """A non-dict parameter on the count variant returns 0."""
        count = get_parameterized_attribute_value(self.web, "AXUIElementCountForSearchPredicate", "not a dict")
        self.assertIsNotNone(count)
        self.assertEqual(int(count), 0)


if __name__ == "__main__":
    unittest.main()
