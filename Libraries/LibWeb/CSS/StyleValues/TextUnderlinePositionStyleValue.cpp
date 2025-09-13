/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>

namespace Web::CSS {

String TextUnderlinePositionStyleValue::to_string(SerializationMode) const
{
    if (m_horizontal == TextUnderlinePositionHorizontal::Auto && m_vertical == TextUnderlinePositionVertical::Auto)
        return "auto"_string;

    if (m_vertical == TextUnderlinePositionVertical::Auto)
        return MUST(String::from_utf8(CSS::to_string(m_horizontal)));

    if (m_horizontal == TextUnderlinePositionHorizontal::Auto)
        return MUST(String::from_utf8(CSS::to_string(m_vertical)));

    return MUST(String::formatted("{} {}", CSS::to_string(m_horizontal), CSS::to_string(m_vertical)));
}

}
