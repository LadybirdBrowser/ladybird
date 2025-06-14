/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GridAutoFlowStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<GridAutoFlowStyleValue const> GridAutoFlowStyleValue::create(Axis axis, Dense dense)
{
    return adopt_ref(*new GridAutoFlowStyleValue(axis, dense));
}

String GridAutoFlowStyleValue::to_string(SerializationMode) const
{
    StringBuilder builder;
    if (m_row)
        builder.append("row"_sv);
    else
        builder.append("column"_sv);
    if (m_dense)
        builder.append(" dense"_sv);
    return MUST(builder.to_string());
}

}
