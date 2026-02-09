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

void GridAutoFlowStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (m_row && !m_dense)
        builder.append("row"sv);
    else if (!m_row)
        builder.append("column"sv);
    if (m_dense) {
        if (!m_row)
            builder.append(' ');
        builder.append("dense"sv);
    }
}

}
