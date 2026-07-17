/*
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpenTypeTaggedStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> OpenTypeTaggedStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto const& absolutized_value = value()->absolutized(computation_context);

    if (absolutized_value == value())
        return *this;

    return OpenTypeTaggedStyleValue::create(mode(), tag(), absolutized_value);
}

void OpenTypeTaggedStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    serialize_a_string(builder, tag());
    switch (this->mode()) {
    case Mode::FontFeatureSettings: {
        // For font-feature-settings, a 1 value is implicit, so we shouldn't output it.
        auto value_string = value()->to_string(mode);
        if (value_string != "1"sv) {
            builder.append(' ');
            value()->serialize(builder, mode);
        }
        break;
    }
    case Mode::FontVariationSettings:
        builder.append(' ');
        value()->serialize(builder, mode);
        break;
    }
}

bool OpenTypeTaggedStyleValue::properties_equal(OpenTypeTaggedStyleValue const& other) const
{
    return other.tag() == tag() && other.value() == value();
}

}
