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

Utf16FlyString GridTemplateAreaStyleValue::cell_name_in(HashMap<Utf16FlyString, GridArea> const& grid_areas, size_t row, size_t column)
{
    for (auto const& [name, area] : grid_areas) {
        if (row >= area.row_start && row < area.row_end && column >= area.column_start && column < area.column_end)
            return name;
    }
    return "."_utf16_fly_string;
}

ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue const> GridTemplateAreaStyleValue::create(HashMap<Utf16FlyString, GridArea> grid_areas, size_t row_count, size_t column_count)
{
    return adopt_ref(*new (nothrow) GridTemplateAreaStyleValue(move(grid_areas), row_count, column_count));
}

}
