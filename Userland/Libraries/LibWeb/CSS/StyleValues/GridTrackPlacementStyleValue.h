/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/GridTrackPlacement.h>

namespace Web::CSS {

class GridTrackPlacementStyleValue final : public StyleValueWithDefaultOperators<GridTrackPlacementStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTrackPlacementStyleValue> create(GridTrackPlacement grid_track_placement);
    virtual ~GridTrackPlacementStyleValue() override = default;

    GridTrackPlacement const& grid_track_placement() const { return m_grid_track_placement; }
    virtual String to_string() const override;

    bool properties_equal(GridTrackPlacementStyleValue const& other) const { return m_grid_track_placement == other.m_grid_track_placement; }

private:
    explicit GridTrackPlacementStyleValue(GridTrackPlacement grid_track_placement)
        : StyleValueWithDefaultOperators(Type::GridTrackPlacement)
        , m_grid_track_placement(grid_track_placement)
    {
    }

    GridTrackPlacement m_grid_track_placement;
};

}
