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
        auto const& list = m_value->counter_definitions.counter_definitions;
        Vector<CounterDefinition> definitions;
        definitions.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i) {
            auto const& definition = list.pointer[i];
            definitions.unchecked_append(CounterDefinition {
                .name = Utf16FlyString::from_raw(definition.name.raw),
                .is_reversed = definition.is_reversed,
                .value = static_cast<StyleValue const*>(definition.value.pointer),
            });
        }
        return definitions;
    }
    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool properties_equal(CounterDefinitionsStyleValue const& other) const;

private:
    explicit CounterDefinitionsStyleValue(Vector<CounterDefinition> counter_definitions)
        : StyleValueWithDefaultOperators(Type::CounterDefinitions, make_counter_definitions_data(counter_definitions))
    {
    }

    static StyleValueFFI::StyleValueData* make_counter_definitions_data(Vector<CounterDefinition> const& counter_definitions)
    {
        // The Rust allocation takes ownership of one leaked reference to each name and one
        // strong reference to each non-null value.
        Vector<StyleValueFFI::RetainedCounterDefinition> ffi_definitions;
        ffi_definitions.ensure_capacity(counter_definitions.size());
        for (auto const& definition : counter_definitions) {
            if (definition.value)
                definition.value->ref();
            ffi_definitions.unchecked_append({ { definition.name.to_raw_leaked() }, definition.is_reversed, { definition.value.ptr() } });
        }
        return StyleValueFFI::rust_style_value_create_counter_definitions(ffi_definitions.data(), ffi_definitions.size());
    }
};

}
