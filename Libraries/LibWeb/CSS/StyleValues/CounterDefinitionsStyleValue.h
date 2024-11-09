/*
 * Copyright (c) 2024, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

struct CounterDefinition {
    FlyString name;
    bool is_reversed;
    ValueComparingRefPtr<CSSStyleValue const> value;
};

/**
 * Holds a list of CounterDefinitions.
 * Shared between counter-increment, counter-reset, and counter-set properties that have (almost) identical grammar.
 */
class CounterDefinitionsStyleValue : public StyleValueWithDefaultOperators<CounterDefinitionsStyleValue> {
public:
    static ValueComparingNonnullRefPtr<CounterDefinitionsStyleValue> create(Vector<CounterDefinition> counter_definitions)
    {
        return adopt_ref(*new (nothrow) CounterDefinitionsStyleValue(move(counter_definitions)));
    }
    virtual ~CounterDefinitionsStyleValue() override = default;

    auto const& counter_definitions() const { return m_counter_definitions; }
    virtual String to_string() const override;

    bool properties_equal(CounterDefinitionsStyleValue const& other) const;

private:
    explicit CounterDefinitionsStyleValue(Vector<CounterDefinition> counter_definitions)
        : StyleValueWithDefaultOperators(Type::CounterDefinitions)
        , m_counter_definitions(move(counter_definitions))
    {
    }

    Vector<CounterDefinition> m_counter_definitions;
};

}
