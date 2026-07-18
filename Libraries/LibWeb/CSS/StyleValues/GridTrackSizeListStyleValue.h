/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/GridTrackSize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridTrackSizeListStyleValue final : public StyleValueWithDefaultOperators<GridTrackSizeListStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> create(CSS::GridTrackSizeList grid_track_size_list);
    virtual ~GridTrackSizeListStyleValue() override = default;

    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> make_auto();
    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue const> make_none();

    CSS::GridTrackSizeList grid_track_size_list() const;

    // NB: Reads the Rust data directly; grid_track_size_list() materializes the whole list.
    bool is_empty() const { return !m_value->grid_track_size_list.is_subgrid && m_value->grid_track_size_list.entries.length == 0; }

    void serialize(StringBuilder&, SerializationMode) const;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(GridTrackSizeListStyleValue const& other) const { return grid_track_size_list() == other.grid_track_size_list(); }

    bool is_computationally_independent() const { return grid_track_size_list().is_computationally_independent(); }

private:
    explicit GridTrackSizeListStyleValue(CSS::GridTrackSizeList grid_track_size_list)
        : StyleValueWithDefaultOperators(Type::GridTrackSizeList, make_grid_track_size_list_data(grid_track_size_list))
    {
    }

    static StyleValueFFI::StyleValueData* make_grid_track_size_list_data(CSS::GridTrackSizeList const&);
};

}
