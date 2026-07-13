/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTemplateAreaStyleValue.h"
#include <AK/Utf16StringBuilder.h>
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue const> GridTemplateAreaStyleValue::create(HashMap<Utf16FlyString, GridArea> grid_areas, size_t row_count, size_t column_count)
{
    return adopt_ref(*new (nothrow) GridTemplateAreaStyleValue(move(grid_areas), row_count, column_count));
}

Utf16FlyString GridTemplateAreaStyleValue::cell_name_at(size_t row, size_t column) const
{
    for (auto const& [name, area] : m_grid_areas) {
        if (row >= area.row_start && row < area.row_end && column >= area.column_start && column < area.column_end)
            return name;
    }
    return "."_utf16_fly_string;
}

void GridTemplateAreaStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (m_row_count == 0) {
        builder.append("none"sv);
        return;
    }

    for (size_t y = 0; y < m_row_count; ++y) {
        if (y != 0)
            builder.append(' ');
        Utf16StringBuilder row_builder;
        for (size_t x = 0; x < m_column_count; ++x) {
            if (x != 0)
                row_builder.append_ascii(' ');
            row_builder.append(cell_name_at(y, x).view());
        }
        auto row = row_builder.to_string();
        serialize_a_string(builder, row.utf16_view());
    }
}

}
