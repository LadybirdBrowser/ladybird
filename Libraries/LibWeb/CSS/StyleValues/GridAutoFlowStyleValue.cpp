/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridAutoFlowStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridAutoFlowStyleValue> GridAutoFlowStyleValue::create(Axis axis, Dense dense)
{
    return adopt_ref(*new GridAutoFlowStyleValue(axis, dense));
}

String GridAutoFlowStyleValue::to_string() const
{
    StringBuilder builder;
    if (m_row)
        builder.append("row"sv);
    else
        builder.append("column"sv);
    if (m_dense)
        builder.append(" dense"sv);
    return MUST(builder.to_string());
}

}
