/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorSchemeStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void ColorSchemeStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    if (schemes().is_empty()) {
        builder.append("normal"sv);
        return;
    }

    bool first = true;
    for (auto const& scheme : schemes()) {
        if (first) {
            first = false;
        } else {
            builder.append(' ');
        }
        builder.append(serialize_an_identifier(scheme));
    }

    if (only())
        builder.append(" only"sv);
}

}
