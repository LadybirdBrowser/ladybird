/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpenTypeTaggedStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String OpenTypeTaggedStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize_a_string(builder, m_tag);
    switch (m_mode) {
    case Mode::FontFeatureSettings: {
        // For font-feature-settings, a 1 value is implicit, so we shouldn't output it.
        auto value_string = m_value->to_string(mode);
        if (value_string != "1"sv)
            builder.appendff(" {}", value_string);
        break;
    }
    case Mode::FontVariationSettings:
        builder.appendff(" {}", m_value->to_string(mode));
        break;
    }

    return builder.to_string_without_validation();
}

bool OpenTypeTaggedStyleValue::properties_equal(OpenTypeTaggedStyleValue const& other) const
{
    return other.tag() == tag() && other.value() == value();
}

}
