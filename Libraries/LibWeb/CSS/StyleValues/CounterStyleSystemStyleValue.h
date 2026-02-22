/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class CounterStyleSystemStyleValue : public StyleValueWithDefaultOperators<CounterStyleSystemStyleValue> {
public:
    NonnullRefPtr<StyleValue const> static create(CounterStyleSystem system)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(system));
    }

    NonnullRefPtr<StyleValue const> static create_fixed(RefPtr<StyleValue const> first_symbol)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(Fixed { move(first_symbol) }));
    }

    NonnullRefPtr<StyleValue const> static create_extends(FlyString name)
    {
        return adopt_ref(*new CounterStyleSystemStyleValue(Extends { move(name) }));
    }

    virtual ~CounterStyleSystemStyleValue() override = default;

    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const& context) const override;
    bool algorithm_differs_from(CounterStyleSystemStyleValue const& other) const;
    bool is_valid_symbol_count(size_t count) const;
    bool is_valid_additive_symbol_count(size_t count) const;

    struct Fixed {
        ValueComparingRefPtr<StyleValue const> first_symbol;
        bool operator==(Fixed const&) const = default;
    };

    struct Extends {
        FlyString name;
        bool operator==(Extends const&) const = default;
    };

    using Value = Variant<CounterStyleSystem, Fixed, Extends>;
    Value const& value() const { return m_value; }

    bool properties_equal(CounterStyleSystemStyleValue const& other) const { return m_value == other.m_value; }

private:
    explicit CounterStyleSystemStyleValue(Variant<CounterStyleSystem, Fixed, Extends> value)
        : StyleValueWithDefaultOperators(Type::CounterStyleSystem)
        , m_value(move(value))
    {
    }

    Value m_value;
};

}
