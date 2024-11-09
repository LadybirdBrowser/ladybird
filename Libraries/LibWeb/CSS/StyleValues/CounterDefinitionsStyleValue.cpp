/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterDefinitionsStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String CounterDefinitionsStyleValue::to_string() const
{
    StringBuilder stb;

    for (auto const& counter_definition : m_counter_definitions) {
        if (!stb.is_empty())
            stb.append(' ');

        if (counter_definition.is_reversed)
            stb.appendff("reversed({})", counter_definition.name);
        else
            stb.append(counter_definition.name);

        if (counter_definition.value)
            stb.appendff(" {}", counter_definition.value->to_string());
    }

    return stb.to_string_without_validation();
}

bool CounterDefinitionsStyleValue::properties_equal(CounterDefinitionsStyleValue const& other) const
{
    if (m_counter_definitions.size() != other.counter_definitions().size())
        return false;

    for (auto i = 0u; i < m_counter_definitions.size(); i++) {
        auto const& ours = m_counter_definitions[i];
        auto const& theirs = other.counter_definitions()[i];
        if (ours.name != theirs.name || ours.is_reversed != theirs.is_reversed || ours.value != theirs.value)
            return false;
    }
    return true;
}

}
