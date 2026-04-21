"""Table cell interface invariants."""

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
from harness import supports_interface  # noqa: E402


class TableCellInterfaceTests(AccessibilityBridgeTestCase):
    FIXTURE = "tables.html"

    def test_table_cells_advertise_tablecell_interface(self):
        """<td>/<th> advertise TableCell interface."""
        cells = find_all_by_role(self.doc, "table cell")
        self.assertGreater(len(cells), 0, "expected table cells")
        for cell in cells:
            self.assertTrue(supports_interface(cell, "tablecell"), f"cell {cell.get_name()!r} must advertise TableCell")

    def test_td_column_index_increments_across_row(self):
        """Cell column index matches its position in its row (0-based)."""
        cells = find_all_by_role(self.doc, "table cell")
        alice = next((c for c in cells if c.get_name() == "Alice"), None)
        age30 = next((c for c in cells if c.get_name() == "30"), None)
        self.assertIsNotNone(alice)
        self.assertIsNotNone(age30)
        # get_position returns (ok, row, column).
        ok_a, alice_row, alice_col = Atspi.TableCell.get_position(alice)
        ok_b, age30_row, age30_col = Atspi.TableCell.get_position(age30)
        self.assertTrue(ok_a)
        self.assertTrue(ok_b)
        self.assertEqual(alice_col, 0, f"Alice is in column 0, got col={alice_col}")
        self.assertEqual(age30_col, 1, f"'30' is in column 1, got col={age30_col}")
        self.assertEqual(alice_row, age30_row, "Alice and '30' are in the same row")

    def test_td_column_span(self):
        """Cell columnExtent reports colspan (default 1 for non-spanning cells)."""
        cells = find_all_by_role(self.doc, "table cell")
        alice = next((c for c in cells if c.get_name() == "Alice"), None)
        spanning = next((c for c in cells if "Spans both columns" in c.get_name()), None)
        self.assertIsNotNone(alice)
        self.assertIsNotNone(spanning)
        self.assertEqual(Atspi.TableCell.get_column_span(alice), 1)
        self.assertEqual(Atspi.TableCell.get_column_span(spanning), 2)


class TableHeaderTests(AccessibilityBridgeTestCase):
    FIXTURE = "tables.html"

    def test_th_is_exposed_as_column_header(self):
        """<th scope='col'> maps to "table column header"."""
        headers = find_all_by_role(self.doc, "table column header")
        names = [h.get_name() for h in headers]
        self.assertIn("Name", names)
        self.assertIn("Age", names)


class AriaGridTests(AccessibilityBridgeTestCase):
    """An ARIA grid or treegrid is a table to the AT (is_table_container_role() in AccessibilityInterface.cpp), so a
    gridcell's TableCell interface counts its row index within them.

    Atspi.TableCell.get_table() isn't checked: Qt's adaptor answers GetTable with a null reference unless the table
    implements QAccessibleTableInterface (AtSpiAdaptor::tableCellInterface()), which the bridge doesn't — for a table
    element either. Orca finds a cell's table by walking up to the table-role ancestor instead."""

    FIXTURE = "grid.html"

    def _cell(self, name):
        cell = next((c for c in find_all_by_role(self.doc, "table cell") if c.get_name() == name), None)
        self.assertIsNotNone(cell, f"expected a table cell named {name!r}")
        return cell

    def test_grid_and_treegrid_are_tables(self):
        """role=grid and role=treegrid both map to "table", like a table element."""
        names = [table.get_name() for table in find_all_by_role(self.doc, "table")]
        self.assertIn("Scores", names)
        self.assertIn("Files", names)

    def test_gridcell_position_counts_rows_across_rowgroups(self):
        """A gridcell's row index is its row's index within the grid — the header row, in its own rowgroup, is row 0."""
        ok, row, column = Atspi.TableCell.get_position(self._cell("Bob"))
        self.assertTrue(ok)
        self.assertEqual((row, column), (2, 0), f"Bob is in row 2, column 0; got row={row} column={column}")
        ok, row, column = Atspi.TableCell.get_position(self._cell("25"))
        self.assertTrue(ok)
        self.assertEqual((row, column), (2, 1), f"'25' is in row 2, column 1; got row={row} column={column}")

    def test_treegrid_cell_position(self):
        """A treegrid's rows are its direct children; the second row's first cell is at row 1, column 0."""
        ok, row, column = Atspi.TableCell.get_position(self._cell("main.cpp"))
        self.assertTrue(ok)
        self.assertEqual((row, column), (1, 0), f"main.cpp is in row 1, column 0; got row={row} column={column}")


if __name__ == "__main__":
    unittest.main()
