/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridTemplateAreaStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridTemplateAreaStyleValue> GridTemplateAreaStyleValue::create(Vector<Vector<String>> grid_template_area)
{
    return adopt_ref(*new (nothrow) GridTemplateAreaStyleValue(grid_template_area));
}

String GridTemplateAreaStyleValue::to_string() const
{
    if (m_grid_template_area.is_empty())
        return "none"_string;

    StringBuilder builder;
    for (size_t y = 0; y < m_grid_template_area.size(); ++y) {
        for (size_t x = 0; x < m_grid_template_area[y].size(); ++x) {
            builder.appendff("{}", m_grid_template_area[y][x]);
            if (x < m_grid_template_area[y].size() - 1)
                builder.append(" "sv);
        }
        if (y < m_grid_template_area.size() - 1)
            builder.append(", "sv);
    }
    return MUST(builder.to_string());
}

}
