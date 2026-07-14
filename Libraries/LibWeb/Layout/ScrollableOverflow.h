/*
 * Copyright (c) 2022-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

// Map from each containing block to the boxes it contains.
using ContainedBoxesMap = HashMap<Box const*, Vector<Box const*>>;

struct ScrollableOverflowMeasurementWork {
    ContainedBoxesMap contained_boxes_map;
    Vector<Box const*> boxes_to_measure;
};

// Walks the committed layout tree once, collecting the boxes whose paintable has no stored overflow
// data (i.e. that need measuring) along with a complete contained-boxes map. The complete map can
// be cached across style-only overflow recalculations and refreshed after layout tree changes.
[[nodiscard]] ScrollableOverflowMeasurementWork collect_scrollable_overflow_measurement_work(Node const& root);

// https://drafts.csswg.org/css-overflow-3/#scrollable-overflow-region
// Measures the scrollable overflow of the given box and stores it on the box's paintable.
// Memoized: if the paintable already has overflow data, the stored value is returned as-is.
CSSPixelRect measure_scrollable_overflow(Box const&, ContainedBoxesMap const&);

}
