/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CounterStyleStyleValue : public StyleValueWithDefaultOperators<CounterStyleStyleValue> {

public:
    struct SymbolsFunction {
        SymbolsType type;
        Vector<FlyString> symbols;

        bool operator==(SymbolsFunction const& other) const = default;
    };

    static ValueComparingNonnullRefPtr<CounterStyleStyleValue const> create(Variant<FlyString, SymbolsFunction> value)
    {
        return adopt_ref(*new (nothrow) CounterStyleStyleValue(move(value)));
    }

    virtual ~CounterStyleStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    RefPtr<CounterStyle const> resolve_counter_style(HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const& registered_counter_styles) const;

    bool properties_equal(CounterStyleStyleValue const& other) const { return m_value == other.m_value; }

private:
    explicit CounterStyleStyleValue(Variant<FlyString, SymbolsFunction> value)
        : StyleValueWithDefaultOperators(Type::CounterStyle)
        , m_value(move(value))
    {
    }

    Variant<FlyString, SymbolsFunction> m_value;
};

}
