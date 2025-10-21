/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterDefinitionsStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String CounterDefinitionsStyleValue::to_string(SerializationMode mode) const
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
            stb.appendff(" {}", counter_definition.value->to_string(mode));
    }

    return stb.to_string_without_validation();
}

ValueComparingNonnullRefPtr<StyleValue const> CounterDefinitionsStyleValue::absolutized(ComputationContext const& computation_context) const
{
    Vector<CounterDefinition> computed_definitions;

    for (auto specified_definition : m_counter_definitions) {
        CounterDefinition computed_definition {
            .name = specified_definition.name,
            .is_reversed = specified_definition.is_reversed,
            .value = nullptr
        };

        if (specified_definition.value)
            computed_definition.value = specified_definition.value->absolutized(computation_context);

        computed_definitions.append(computed_definition);
    }

    return CounterDefinitionsStyleValue::create(computed_definitions);
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
