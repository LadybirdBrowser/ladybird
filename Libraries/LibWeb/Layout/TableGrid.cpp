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

    size_t x_width = 0;
    size_t y_height = 0;
    size_t y_current = 0;
    size_t max_cell_x = 0;
    size_t max_cell_y = 0;

    // Implements https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
    auto process_row = [&table_grid, &cells, &rows, &x_width, &y_height, &y_current, &max_cell_x, &max_cell_y](Box const& row, Optional<Box&> row_group = {}) {
        // 1. If yheight is equal to ycurrent, then increase yheight by 1. (ycurrent is never greater than yheight.)
        if (y_height == y_current)
            y_height++;

        // 2. Let xcurrent be 0.
        size_t x_current = 0;

        // FIXME: 3. Run the algorithm for growing downward-growing cells.

        // 4. If the tr element being processed has no td or th element children, then increase ycurrent by 1, abort
        //    this set of steps, and return to the algorithm above.
        // NB: The remaining steps already accomplish the same thing in this case.

        // 5. Let current cell be the first td or th element child in the tr element being processed.
        for (auto* child = row.first_child(); child; child = child->next_sibling()) {
            // NB: This actually applies to children with `display: table-cell`, not just td/th elements.
            if (!child->display().is_table_cell())
                continue;

            auto& current_cell = as<Box>(*child);

            // 6. Cells: While x_current is less than x_width and the slot with coordinate (x_current, y_current)
            //    already has a cell assigned to it, increase x_current by 1.
            while (x_current < x_width && table_grid.m_occupancy_grid.contains(GridPosition { x_current, y_current }))
                x_current++;

            // 7. If xcurrent is equal to xwidth, increase xwidth by 1. (xcurrent is never greater than xwidth.)
            if (x_current == x_width)
                x_width++;

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

            // 12. If xwidth < xcurrent+colspan, then let xwidth be xcurrent+colspan.
            if (x_width < x_current + colspan)
                x_width = x_current + colspan;

            // 13. If yheight < ycurrent+rowspan, then let yheight be ycurrent+rowspan.
            if (y_height < y_current + rowspan)
                y_height = y_current + rowspan;

            // 14. Let the slots with coordinates (x, y) such that xcurrent ≤ x < xcurrent+colspan and
            //     ycurrent ≤ y < ycurrent+rowspan be covered by a new cell c, anchored at (xcurrent, ycurrent),
            //     which has width colspan and height rowspan, corresponding to the current cell element.
            //     If the current cell element is a th element, let this new cell c be a header cell;
            //     otherwise, let it be a data cell.
            //     To establish which header cells apply to the current cell element, use the algorithm for
            //     assigning header cells described in the next section.
            //     If any of the slots involved already had a cell covering them, then this is a table model error.
            //     Those slots now have two cells overlapping.
            // NB: We don't distinguish between header and data cells here.
            for (size_t y = y_current; y < y_current + rowspan; y++)
                for (size_t x = x_current; x < x_current + colspan; x++)
                    table_grid.m_occupancy_grid.set(GridPosition { x, y }, true);
            cells.append(Cell { current_cell, x_current, y_current, colspan, rowspan });
            max_cell_x = max(x_current, max_cell_x);
            max_cell_y = max(y_current, max_cell_y);

            // 15. If cell grows downward is true, then add the tuple {c, xcurrent, colspan} to the list of downward-growing cells.
            if (cell_grows_downward) {
                // FIXME: Add the tuple.
            }

            // 16. Increase xcurrent by colspan.
            x_current += colspan;

            // NB: Step 17 is handled below, outside of this loop.

            // 18. Let current cell be the next td or th element child in the tr element being processed.
            // 19. Return to the step labeled cells.
            // NB: Handled by the loop.
        }

        // 17. If current cell is the last td or th element child in the tr element being processed, then increase
        //    ycurrent by 1, abort this set of steps, and return to the algorithm above.
        rows.append(Row {
            .box = row,
            .is_collapsed = row.computed_values().visibility() == CSS::Visibility::Collapse
                || (row_group.has_value() && row_group->computed_values().visibility() == CSS::Visibility::Collapse),
        });
        y_current++;
    };

    auto process_col_group = [&](auto& col_group) {
        auto dom_node = col_group.dom_node();
        dom_node->for_each_in_subtree([&](auto& descendant) {
            // NB: Called during table layout tree construction.
            if (descendant.unsafe_layout_node() && descendant.unsafe_layout_node()->display().is_table_column()) {
                u32 span = 1;
                if (auto const* col_element = as_if<HTML::HTMLTableColElement>(descendant))
                    span = col_element->span();
                x_width += span;
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

    table_grid.m_column_count = x_width;

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
