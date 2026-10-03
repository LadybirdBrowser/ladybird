"""ARIA grid and treegrid: tables to VoiceOver, with their rows counted across rowgroups.

The wrapper gives table, grid, and treegrid NSAccessibilityTableRole alike (is_table_container_role() in
LadybirdAccessibilityElement.mm), as WebKit and Blink both do — so a row's AXIndex and a cell's AXRowIndexRange come
from the enclosing grid, not from a table element that isn't there."""

from __future__ import annotations

import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from ApplicationServices import kAXRoleAttribute  # noqa: E402
from ApplicationServices import kAXValueAttribute  # noqa: E402
from harness import AccessibilityBridgeMacTestCase  # noqa: E402
from harness import find_all_by_role  # noqa: E402
from harness import walk  # noqa: E402
from harness.ladybird import _ax_attr  # noqa: E402


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
    """Unwrap an AXValue CFRange into (location, length) from its description."""
    m = re.search(r"location:(\d+)\s+length:(\d+)", str(value))
    if m is None:
        return None
    return int(m.group(1)), int(m.group(2))


class AriaGridTests(AccessibilityBridgeMacTestCase):
    FIXTURE = "grid.html"

    def _table_containing(self, text):
        tables = find_all_by_role(self.web, "AXTable")
        for table in tables:
            if text in _static_text_values(table):
                return table
        self.fail(f"no AXTable contains the text {text!r}; found {len(tables)} AXTable elements")

    def _cell_with_text(self, text):
        for cell in find_all_by_role(self.web, "AXCell"):
            if text in _static_text_values(cell):
                return cell
        self.fail(f"no AXCell contains the text {text!r}")

    def test_grid_and_treegrid_are_tables(self):
        """A role=grid and a role=treegrid are both AXTable, like a table element."""
        self.assertIsNotNone(self._table_containing("Alice"))
        self.assertIsNotNone(self._table_containing("main.cpp"))

    def test_grid_rows_are_counted_across_rowgroups(self):
        """The grid's header row and its two body rows, in two rowgroups, are three rows of one table."""
        grid = self._table_containing("Alice")
        rows = _ax_attr(grid, "AXRows") or []
        self.assertEqual(len(rows), 3, f"expected the grid's three rows, got {len(rows)}")
        self.assertEqual(_ax_attr(grid, "AXRowCount"), 3)
        self.assertEqual(_ax_attr(grid, "AXColumnCount"), 2)

    def test_gridcell_row_index_comes_from_the_grid(self):
        """A gridcell's AXRowIndexRange is its row's index within the grid — the header row is row 0."""
        self.assertEqual(_parse_cf_range(_ax_attr(self._cell_with_text("Alice"), "AXRowIndexRange")), (1, 1))
        self.assertEqual(_parse_cf_range(_ax_attr(self._cell_with_text("Bob"), "AXRowIndexRange")), (2, 1))
        self.assertEqual(_parse_cf_range(_ax_attr(self._cell_with_text("25"), "AXColumnIndexRange")), (1, 1))

    def test_treegrid_cell_row_index_comes_from_the_treegrid(self):
        """A treegrid's rows are its direct children; the second row's cell is in row 1."""
        self.assertEqual(_parse_cf_range(_ax_attr(self._cell_with_text("main.cpp"), "AXRowIndexRange")), (1, 1))


class TreegridRowLevelTests(AccessibilityBridgeMacTestCase):
    """A row's aria-level is its depth: AXDisclosureLevel, 0-based as WebKit and Blink convert it, and never its
    AXValue, which carries a heading's level only."""

    FIXTURE = "grid.html"

    def _row_containing(self, text):
        for row in find_all_by_role(self.web, "AXRow"):
            if text in _static_text_values(row):
                return row
        self.fail(f"no AXRow contains the text {text!r}")

    def test_row_level_is_its_disclosure_level_not_its_value(self):
        child = self._row_containing("main.cpp")
        self.assertIsNone(_ax_attr(child, kAXValueAttribute), "the row's aria-level leaked into its AXValue")
        self.assertEqual(_ax_attr(child, "AXDisclosureLevel"), 1)
        self.assertEqual(_ax_attr(self._row_containing("src"), "AXDisclosureLevel"), 0)


class HeaderCellTests(AccessibilityBridgeMacTestCase):
    """A th is a cell to VoiceOver (AXCell, as WebKit, Blink and Gecko expose column and row headers), so table
    navigation reaches it and its text can name the column."""

    FIXTURE = "tables.html"

    def test_header_cells_are_cells(self):
        texts = set()
        for cell in find_all_by_role(self.web, "AXCell"):
            texts.update(_static_text_values(cell))
        self.assertTrue({"Name", "Age"} <= texts, f"the th cells aren't AXCell; AXCell texts: {sorted(texts)}")


if __name__ == "__main__":
    unittest.main()
