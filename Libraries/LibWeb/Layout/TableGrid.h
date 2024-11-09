/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibWeb/Layout/Box.h>

namespace Web::Layout {

class TableGrid {
public:
    struct GridPosition {
        size_t x;
        size_t y;
        inline bool operator==(GridPosition const&) const = default;
    };

    struct Row {
        JS::NonnullGCPtr<Box const> box;
        CSSPixels base_height { 0 };
        CSSPixels reference_height { 0 };
        CSSPixels final_height { 0 };
        CSSPixels baseline { 0 };
        CSSPixels min_size { 0 };
        CSSPixels max_size { 0 };
        bool has_intrinsic_percentage { false };
        double intrinsic_percentage { 0 };
        // Store whether the row is constrained: https://www.w3.org/TR/css-tables-3/#constrainedness
        bool is_constrained { false };
    };

    struct Cell {
        JS::NonnullGCPtr<Box const> box;
        size_t column_index;
        size_t row_index;
        size_t column_span;
        size_t row_span;
        CSSPixels baseline { 0 };
        CSSPixels outer_min_width { 0 };
        CSSPixels outer_max_width { 0 };
        CSSPixels outer_min_height { 0 };
        CSSPixels outer_max_height { 0 };
    };

    // Calculate and return the grid and also rows and cells as output parameters.
    static TableGrid calculate_row_column_grid(Box const&, Vector<Cell>&, Vector<Row>&);
    // Overload for callers that don't care about rows and cells (currently the layout tree builder).
    static TableGrid calculate_row_column_grid(Box const& box);

    size_t column_count() const { return m_column_count; }
    HashMap<GridPosition, bool> const& occupancy_grid() const { return m_occupancy_grid; }

    static bool is_table_row_group(Box const& box)
    {
        auto const& display = box.display();
        return display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group();
    }

    static bool is_table_row(Box const& box)
    {
        return box.display().is_table_row();
    }

    static bool is_table_column_group(Box const& box)
    {
        return box.display().is_table_column_group();
    }

    template<typename Matcher, typename Callback>
    static void for_each_child_box_matching(Box const& parent, Matcher matcher, Callback callback)
    {
        parent.for_each_child_of_type<Box>([&](Box const& child_box) {
            if (matcher(child_box))
                callback(child_box);
            return IterationDecision::Continue;
        });
    }

private:
    size_t m_column_count { 0 };
    HashMap<GridPosition, bool> m_occupancy_grid;
};

}

namespace AK {

template<>
struct Traits<Web::Layout::TableGrid::GridPosition> : public DefaultTraits<Web::Layout::TableGrid::GridPosition> {
    static unsigned hash(Web::Layout::TableGrid::GridPosition const& key)
    {
        return pair_int_hash(key.x, key.y);
    }
};

}
