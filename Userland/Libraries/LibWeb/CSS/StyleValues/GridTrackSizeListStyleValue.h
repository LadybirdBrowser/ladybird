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
#include <LibWeb/CSS/GridTrackSize.h>

namespace Web::CSS {

class GridTrackSizeListStyleValue final : public StyleValueWithDefaultOperators<GridTrackSizeListStyleValue> {
public:
    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue> create(CSS::GridTrackSizeList grid_track_size_list);
    virtual ~GridTrackSizeListStyleValue() override = default;

    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue> make_auto();
    static ValueComparingNonnullRefPtr<GridTrackSizeListStyleValue> make_none();

    CSS::GridTrackSizeList grid_track_size_list() const { return m_grid_track_size_list; }

    virtual String to_string() const override;

    bool properties_equal(GridTrackSizeListStyleValue const& other) const { return m_grid_track_size_list == other.m_grid_track_size_list; }

private:
    explicit GridTrackSizeListStyleValue(CSS::GridTrackSizeList grid_track_size_list)
        : StyleValueWithDefaultOperators(Type::GridTrackSizeList)
        , m_grid_track_size_list(grid_track_size_list)
    {
    }

    CSS::GridTrackSizeList m_grid_track_size_list;
};

}
