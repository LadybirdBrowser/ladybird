/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ColorSchemeStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String ColorSchemeStyleValue::to_string(SerializationMode) const
{
    if (schemes().is_empty())
        return "normal"_string;

    StringBuilder builder;
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

    return MUST(builder.to_string());
}

}
