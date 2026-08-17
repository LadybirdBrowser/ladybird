/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

struct CollapsedBorderEdge {
    CSS::BorderData border;
    u32 source_order { 0 };
};

struct CollapsedTableBorders {
    Vector<CSSPixels> row_offsets;
    Vector<CSSPixels> column_offsets;
    Vector<CollapsedBorderEdge> horizontal_edges;
    Vector<CollapsedBorderEdge> vertical_edges;

    size_t row_count() const { return row_offsets.size() - 1; }
    size_t column_count() const { return column_offsets.size() - 1; }
};

}
