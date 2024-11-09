/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>

namespace Web::CSS {

String TransitionStyleValue::to_string() const
{
    StringBuilder builder;
    bool first = true;
    for (auto const& transition : m_transitions) {
        if (!first)
            builder.append(", "sv);
        first = false;
        builder.appendff("{} {} {} {}", transition.property_name->to_string(), transition.duration, transition.easing->to_string(), transition.delay);
    }

    return MUST(builder.to_string());
}

bool TransitionStyleValue::properties_equal(TransitionStyleValue const& other) const
{
    if (m_transitions.size() != other.m_transitions.size())
        return false;

    for (size_t i = 0; i < m_transitions.size(); i++) {
        if (m_transitions[i] != other.m_transitions[i])
            return false;
    }

    return true;
}

}
