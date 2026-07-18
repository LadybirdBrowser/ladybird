/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableColElement.h>
#include <LibWeb/Layout/TableGrid.h>

namespace Web::Layout {

TableGrid TableGrid::calculate_row_column_grid(Box const& box, Vector<Cell>& cells, Vector<Row>& rows)
{
    // Implements https://html.spec.whatwg.org/multipage/tables.html#forming-a-table
    TableGrid table_grid;

    size_t column_count = 0;
    size_t row_count = 0;
    size_t current_row = 0;
    size_t maximum_cell_column_index = 0;
    size_t maximum_cell_row_index = 0;

    // Implements https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
    auto process_row = [&table_grid, &cells, &rows, &column_count, &row_count, &current_row, &maximum_cell_column_index, &maximum_cell_row_index](Box const& row, Optional<Box&> row_group = {}) {
        // 1. If the row count equals the current row, increase the row count by 1.
        if (row_count == current_row)
            row_count++;

        // 2. Let the current column be 0.
        size_t current_column = 0;

        // FIXME: 3. Run the algorithm for growing downward-growing cells.

        // 4. If the tr element being processed has no td or th element children, then increase ycurrent by 1, abort
        //    this set of steps, and return to the algorithm above.
        // NB: The remaining steps already accomplish the same thing in this case.

        // 5. Let current cell be the first td or th element child in the tr element being processed.
        for (auto child = row.first_child(); child; child = child->next_sibling()) {
            // NB: This actually applies to children with `display: table-cell`, not just td/th elements.
            auto const* child_box = as_if<Box>(*child);
            if (!child_box || !child_box->display().is_table_cell())
                continue;

            auto& current_cell = const_cast<Box&>(*child_box);

            // 6. Cells: While the current column is within the grid and its slot in the current row
            //    already has a cell assigned to it, increase the current column by 1.
            while (current_column < column_count && table_grid.m_occupancy_grid.contains(GridPosition { current_column, current_row }))
                current_column++;

            // 7. If the current column equals the column count, increase the column count by 1.
            if (current_column == column_count)
                column_count++;

            // NB: Steps 8 and 9 are implemented in HTMLTableCellElement.col_span() and HTMLTableCellElement.row_spam() respectively.
            size_t colspan = 1;
            size_t rowspan = 1;
            if (auto* table_cell = as_if<HTML::HTMLTableCellElement>(current_cell.dom_node())) {
                colspan = table_cell->col_span();
                rowspan = table_cell->row_span();
            }

            // 10. Let cell grows downward be false.
            auto cell_grows_downward = false;

            // 11. If rowspan is zero, then set cell grows downward to true and set rowspan to 1.
            if (rowspan == 0) {
                cell_grows_downward = true;
                rowspan = 1;
            }

            // 12. Extend the column count to include the cell's column span.
            if (column_count < current_column + colspan)
                column_count = current_column + colspan;

            // 13. Extend the row count to include the cell's row span.
            if (row_count < current_row + rowspan)
                row_count = current_row + rowspan;

            // 14. Let the slots within the cell's column and row spans be covered by a new cell,
            //     anchored at the current column and row,
            //     which has width colspan and height rowspan, corresponding to the current cell element.
            //     If the current cell element is a th element, let this new cell c be a header cell;
            //     otherwise, let it be a data cell.
            //     To establish which header cells apply to the current cell element, use the algorithm for
            //     assigning header cells described in the next section.
            //     If any of the slots involved already had a cell covering them, then this is a table model error.
            //     Those slots now have two cells overlapping.
            // NB: We don't distinguish between header and data cells here.
            for (size_t row_index = current_row; row_index < current_row + rowspan; row_index++)
                for (size_t column_index = current_column; column_index < current_column + colspan; column_index++)
                    table_grid.m_occupancy_grid.set(GridPosition { column_index, row_index }, true);
            cells.append(Cell { current_cell, current_column, current_row, colspan, rowspan });
            maximum_cell_column_index = max(current_column, maximum_cell_column_index);
            maximum_cell_row_index = max(current_row, maximum_cell_row_index);

            // 15. If cell grows downward is true, add the cell, current column, and column span to the list of downward-growing cells.
            if (cell_grows_downward) {
                // FIXME: Add the tuple.
            }

            // 16. Increase the current column by the column span.
            current_column += colspan;

            // NB: Step 17 is handled below, outside of this loop.

            // 18. Let current cell be the next td or th element child in the tr element being processed.
            // 19. Return to the step labeled cells.
            // NB: Handled by the loop.
        }

        // 17. If current cell is the last td or th element child in the tr element being processed, then increase
        //    the current row by 1, abort this set of steps, and return to the algorithm above.
        rows.append(Row {
            .box = row,
            .is_collapsed = row.computed_values().visibility() == CSS::Visibility::Collapse
                || (row_group.has_value() && row_group->computed_values().visibility() == CSS::Visibility::Collapse),
        });
        current_row++;
    };

    auto process_col_group = [&](auto& col_group) {
        col_group.template for_each_in_subtree_of_type<Box>([&](auto& descendant_box) {
            if (descendant_box.display().is_table_column()) {
                u32 span = 1;
                if (auto const* col_element = as_if<HTML::HTMLTableColElement>(descendant_box.dom_node()))
                    span = col_element->span();
                column_count += span;
            }
            return TraversalDecision::Continue;
        });
    };

    for_each_child_box_matching(box, is_table_column_group, [&](auto& column_group_box) {
        process_col_group(column_group_box);
    });

    auto process_row_group = [&](auto& row_group) {
        for_each_child_box_matching(row_group, is_table_row, [&](auto& row_box) {
            process_row(row_box, row_group);
            return IterationDecision::Continue;
        });
    };

    box.for_each_child_of_type<Box>([&](auto& child) {
        if (is_table_row_group(child))
            process_row_group(child);
        else if (is_table_row(child))
            process_row(child);
        return IterationDecision::Continue;
    });

    table_grid.m_column_count = column_count;

    for (auto& cell : cells) {
        // Clip spans to the end of the table.
        cell.row_span = min(cell.row_span, rows.size() - cell.row_index);
        cell.column_span = min(cell.column_span, table_grid.m_column_count - cell.column_index);
    }

    return table_grid;
}

TableGrid TableGrid::calculate_row_column_grid(Box const& box)
{
    Vector<Cell> cells;
    Vector<Row> rows;
    return calculate_row_column_grid(box, cells, rows);
}

}
