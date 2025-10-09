/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTemplateAreaStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue const> GridTemplateAreaStyleValue::create(Vector<Vector<String>> grid_template_area)
{
    return adopt_ref(*new (nothrow) GridTemplateAreaStyleValue(move(grid_template_area)));
}

String GridTemplateAreaStyleValue::to_string(SerializationMode) const
{
    if (m_grid_template_area.is_empty())
        return "none"_string;

    StringBuilder builder;
    for (size_t y = 0; y < m_grid_template_area.size(); ++y) {
        if (y != 0)
            builder.append(' ');
        StringBuilder row_builder;
        for (size_t x = 0; x < m_grid_template_area[y].size(); ++x) {
            if (x != 0)
                row_builder.append(' ');
            row_builder.appendff("{}", m_grid_template_area[y][x]);
        }
        serialize_a_string(builder, row_builder.string_view());
    }
    return MUST(builder.to_string());
}

}
