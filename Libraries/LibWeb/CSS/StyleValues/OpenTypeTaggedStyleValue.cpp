/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OpenTypeTaggedStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String OpenTypeTaggedStyleValue::to_string() const
{
    StringBuilder builder;
    serialize_a_string(builder, m_tag);
    // FIXME: For font-feature-settings, a 1 value is implicit, so we shouldn't output it.
    builder.appendff(" {}", m_value->to_string());

    return builder.to_string_without_validation();
}

bool OpenTypeTaggedStyleValue::properties_equal(OpenTypeTaggedStyleValue const& other) const
{
    return other.tag() == tag() && other.value() == value();
}

}
