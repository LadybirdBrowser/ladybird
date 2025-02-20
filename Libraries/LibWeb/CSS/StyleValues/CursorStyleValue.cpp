/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CursorStyleValue.h"
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>

namespace Web::CSS {

String CursorStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;

    builder.append(m_properties.image->to_string(mode));

    if (m_properties.x.has_value()) {
        VERIFY(m_properties.y.has_value());
        builder.appendff(" {} {}", m_properties.x->to_string(), m_properties.y->to_string());
    }

    return builder.to_string_without_validation();
}

}
