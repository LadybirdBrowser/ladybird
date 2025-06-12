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

    size_t x_width = 0, y_height = 0;
    size_t x_current = 0, y_current = 0;
    size_t max_cell_x = 0, max_cell_y = 0;

    // Implements https://html.spec.whatwg.org/multipage/tables.html#algorithm-for-processing-rows
    auto process_row = [&](auto& row) {
        if (y_height == y_current)
            y_height++;

        x_current = 0;

        for (auto* child = row.first_child(); child; child = child->next_sibling()) {
            if (child->display().is_table_cell()) {
                // Cells: While x_current is less than x_width and the slot with coordinate (x_current, y_current) already has a cell assigned to it, increase x_current by 1.
                while (x_current < x_width && table_grid.m_occupancy_grid.contains(GridPosition { x_current, y_current }))
                    x_current++;

                Box const* box = static_cast<Box const*>(child);
                if (x_current == x_width)
                    x_width++;

                size_t colspan = 1, rowspan = 1;
                if (box->dom_node() && is<HTML::HTMLTableCellElement>(*box->dom_node())) {
                    auto const& node = static_cast<HTML::HTMLTableCellElement const&>(*box->dom_node());
                    colspan = node.col_span();
                    rowspan = node.row_span();
                }

                if (x_width < x_current + colspan)
                    x_width = x_current + colspan;
                if (y_height < y_current + rowspan)
                    y_height = y_current + rowspan;

                for (size_t y = y_current; y < y_current + rowspan; y++)
                    for (size_t x = x_current; x < x_current + colspan; x++)
                        table_grid.m_occupancy_grid.set(GridPosition { x, y }, true);
                cells.append(Cell { *box, x_current, y_current, colspan, rowspan });
                max_cell_x = max(x_current, max_cell_x);
                max_cell_y = max(y_current, max_cell_y);

                x_current += colspan;
            }
        }

        rows.append(Row { row });
        y_current++;
    };

    auto process_col_group = [&](auto& col_group) {
        auto dom_node = col_group.dom_node();
        dom_node->template for_each_in_subtree_of_type<HTML::HTMLTableColElement>([&](auto&) {
            x_width += 1;
            return TraversalDecision::Continue;
        });
    };

    for_each_child_box_matching(box, is_table_column_group, [&](auto& column_group_box) {
        process_col_group(column_group_box);
    });

    auto process_row_group = [&](auto& row_group) {
        for_each_child_box_matching(row_group, is_table_row, [&](auto& row_box) {
            process_row(row_box);
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
