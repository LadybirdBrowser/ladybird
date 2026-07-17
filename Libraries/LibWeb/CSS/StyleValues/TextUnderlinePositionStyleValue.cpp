/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>

namespace Web::CSS {

void TextUnderlinePositionStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (horizontal() == TextUnderlinePositionHorizontal::Auto && vertical() == TextUnderlinePositionVertical::Auto) {
        builder.append("auto"sv);
        return;
    }

    if (vertical() == TextUnderlinePositionVertical::Auto) {
        builder.append(CSS::to_string(horizontal()));
        return;
    }

    if (horizontal() == TextUnderlinePositionHorizontal::Auto) {
        builder.append(CSS::to_string(vertical()));
        return;
    }

    builder.appendff("{} {}", CSS::to_string(horizontal()), CSS::to_string(vertical()));
}

}
