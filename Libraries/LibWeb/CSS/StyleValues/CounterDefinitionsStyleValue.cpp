/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterDefinitionsStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

CounterDefinitionsStyleValue::CounterDefinitionsStyleValue(StyleValueFFI::StyleValueData const* data)
    : StyleValueWithDefaultOperators(Type::CounterDefinitions, data)
{
    auto const& list = data->counter_definitions.counter_definitions;
    m_counter_definitions.ensure_capacity(list.length);
    for (size_t i = 0; i < list.length; ++i) {
        auto const& definition = list.pointer[i];
        ValueComparingRefPtr<StyleValue const> value;
        if (auto const* value_data = static_cast<StyleValueFFI::StyleValueData const*>(definition.value.pointer))
            value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value_data));
        m_counter_definitions.unchecked_append(CounterDefinition {
            .name = Utf16FlyString::from_raw(definition.name.raw),
            .is_reversed = definition.is_reversed,
            .value = move(value),
        });
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
