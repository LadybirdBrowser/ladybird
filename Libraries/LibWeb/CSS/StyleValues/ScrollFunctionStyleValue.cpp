/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ScrollFunctionStyleValue.h"

namespace Web::CSS {

String ScrollFunctionStyleValue::to_string(SerializationMode) const
{
    StringBuilder builder;
    builder.append("scroll("sv);

    if (m_scroller != Scroller::Nearest)
        builder.append(CSS::to_string(m_scroller));

    if (m_axis != Axis::Block) {
        if (m_scroller != Scroller::Nearest)
            builder.append(' ');
        builder.append(CSS::to_string(m_axis));
    }

    builder.append(')');
    return builder.to_string_without_validation();
}

}
