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
    if (is_row() && !is_dense())
        builder.append("row"sv);
    else if (!is_row())
        builder.append("column"sv);
    if (is_dense()) {
        if (!is_row())
            builder.append(' ');
        builder.append("dense"sv);
    }
}

}
