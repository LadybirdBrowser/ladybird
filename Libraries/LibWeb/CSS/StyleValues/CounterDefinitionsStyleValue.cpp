/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterDefinitionsStyleValue.h"

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
            .name = css_string_from_rust(&definition.name),
            .is_reversed = definition.is_reversed,
            .value = wrap_rust_child_or_null(definition.value),
        });
    }
    return counter_definitions;
}

}
