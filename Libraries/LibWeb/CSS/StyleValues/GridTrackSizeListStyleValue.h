/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class GridTrackSizeListStyleValue final : public StyleValueWithDefaultOperators<GridTrackSizeListStyleValue> {
public:
    virtual ~GridTrackSizeListStyleValue() override = default;

    bool is_empty() const { return !m_value->grid_track_size_list.is_subgrid && m_value->grid_track_size_list.entries.length == 0; }

private:
    friend class StyleValue;

    explicit GridTrackSizeListStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::GridTrackSizeList, data)
    {
    }
};

}
