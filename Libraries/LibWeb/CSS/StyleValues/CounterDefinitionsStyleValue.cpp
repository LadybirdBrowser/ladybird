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
}

Vector<CounterDefinition> CounterDefinitionsStyleValue::counter_definitions() const
{
    auto const& list = m_value->counter_definitions.counter_definitions;
    Vector<CounterDefinition> counter_definitions;
    counter_definitions.ensure_capacity(list.length);
    for (size_t i = 0; i < list.length; ++i) {
        auto const& definition = list.pointer[i];
        counter_definitions.unchecked_append(CounterDefinition {
            .name = Utf16FlyString::from_raw(definition.name.raw),
            .is_reversed = definition.is_reversed,
            .value = wrap_rust_child_or_null(definition.value),
        });
    }
    return counter_definitions;
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

}
