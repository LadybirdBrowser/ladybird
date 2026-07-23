/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

struct CounterDefinition {
    Utf16FlyString name;
    bool is_reversed;
    ValueComparingRefPtr<StyleValue const> value;
};

/**
 * Holds a list of CounterDefinitions.
 * Shared between counter-increment, counter-reset, and counter-set properties that have (almost) identical grammar.
 */
class WEB_API CounterDefinitionsStyleValue : public StyleValueWithDefaultOperators<CounterDefinitionsStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CounterDefinitionsStyleValue const> create(Vector<CounterDefinition> counter_definitions)
    {
        return adopt_ref(*new (nothrow) CounterDefinitionsStyleValue(move(counter_definitions)));
    }
    virtual ~CounterDefinitionsStyleValue() override = default;

    Vector<CounterDefinition> counter_definitions() const
    {
        return m_counter_definitions;
    }
    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(CounterDefinitionsStyleValue const& other) const;

private:
    friend class StyleValue;

    explicit CounterDefinitionsStyleValue(Vector<CounterDefinition> counter_definitions)
        : StyleValueWithDefaultOperators(Type::CounterDefinitions, make_counter_definitions_data(counter_definitions))
        , m_counter_definitions(move(counter_definitions))
    {
    }

    explicit CounterDefinitionsStyleValue(StyleValueFFI::StyleValueData const*);

    static StyleValueFFI::StyleValueData const* make_counter_definitions_data(Vector<CounterDefinition> const& counter_definitions)
    {
        Vector<StyleValueFFI::RetainedCounterDefinition> ffi_definitions;
        ffi_definitions.ensure_capacity(counter_definitions.size());
        for (auto const& definition : counter_definitions) {
            auto const* value_data = definition.value ? StyleValueFFI::rust_style_value_retain(definition.value->rust_style_value_data()) : nullptr;
            ffi_definitions.unchecked_append({ { definition.name.to_raw_leaked() }, definition.is_reversed, { value_data } });
        }
        return StyleValueFFI::rust_style_value_create_counter_definitions(ffi_definitions.data(), ffi_definitions.size());
    }

    Vector<CounterDefinition> m_counter_definitions;
};

}
