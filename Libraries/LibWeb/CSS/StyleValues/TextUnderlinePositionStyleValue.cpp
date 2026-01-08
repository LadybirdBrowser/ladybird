/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>

namespace Web::CSS {

void TextUnderlinePositionStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (m_horizontal == TextUnderlinePositionHorizontal::Auto && m_vertical == TextUnderlinePositionVertical::Auto) {
        builder.append("auto"sv);
        return;
    }

    if (m_vertical == TextUnderlinePositionVertical::Auto) {
        builder.append(CSS::to_string(m_horizontal));
        return;
    }

    if (m_horizontal == TextUnderlinePositionHorizontal::Auto) {
        builder.append(CSS::to_string(m_vertical));
        return;
    }

    builder.appendff("{} {}", CSS::to_string(m_horizontal), CSS::to_string(m_vertical));
}

}
