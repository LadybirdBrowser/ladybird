/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CounterStyleStyleValue : public StyleValueWithDefaultOperators<CounterStyleStyleValue> {

public:
    struct SymbolsFunction {
        SymbolsType type;
        Vector<Utf16FlyString> symbols;

        bool operator==(SymbolsFunction const& other) const = default;
    };

    static ValueComparingNonnullRefPtr<CounterStyleStyleValue const> create(Variant<Utf16FlyString, SymbolsFunction> value)
    {
        return adopt_ref(*new (nothrow) CounterStyleStyleValue(move(value)));
    }

    virtual ~CounterStyleStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    RefPtr<CounterStyle const> resolve_counter_style(StyleScope const&) const;
    Variant<Utf16FlyString, SymbolsFunction> const& value() const { return m_value; }

    bool properties_equal(CounterStyleStyleValue const& other) const { return m_value == other.m_value; }

    virtual bool is_computationally_independent() const override { return true; }

private:
    explicit CounterStyleStyleValue(Variant<Utf16FlyString, SymbolsFunction> value)
        : StyleValueWithDefaultOperators(Type::CounterStyle)
        , m_value(move(value))
    {
    }

    Variant<Utf16FlyString, SymbolsFunction> m_value;
};

}
