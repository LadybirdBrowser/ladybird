/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterDefinitionsStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void CounterDefinitionsStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    bool first = true;
    for (auto const& counter_definition : counter_definitions()) {
        if (first)
            first = false;
        else
            builder.append(' ');

        if (counter_definition.is_reversed)
            builder.appendff("reversed({})", serialize_an_identifier(counter_definition.name));
        else
            builder.append(serialize_an_identifier(counter_definition.name));

        if (counter_definition.value) {
            builder.append(' ');
            counter_definition.value->serialize(builder, mode);
        }
    }
}

ValueComparingNonnullRefPtr<StyleValue const> CounterDefinitionsStyleValue::absolutized(ComputationContext const& computation_context) const
{
    Vector<CounterDefinition> computed_definitions;

    for (auto specified_definition : counter_definitions()) {
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
    auto our_definitions = counter_definitions();
    auto their_definitions = other.counter_definitions();
    if (our_definitions.size() != their_definitions.size())
        return false;

    for (auto i = 0u; i < our_definitions.size(); i++) {
        auto const& ours = our_definitions[i];
        auto const& theirs = their_definitions[i];
        if (ours.name != theirs.name || ours.is_reversed != theirs.is_reversed || ours.value != theirs.value)
            return false;
    }
    return true;
}

}
