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
    static ValueComparingNonnullRefPtr<CounterStyleStyleValue const> create(FlyString name)
    {
        return adopt_ref(*new (nothrow) CounterStyleStyleValue(move(name)));
    }

    virtual ~CounterStyleStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    RefPtr<CounterStyle const> resolve_counter_style(HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const& registered_counter_styles) const;

    bool properties_equal(CounterStyleStyleValue const& other) const { return m_name == other.m_name; }

private:
    explicit CounterStyleStyleValue(FlyString name)
        : StyleValueWithDefaultOperators(Type::CounterStyle)
        , m_name(move(name))
    {
    }

    FlyString m_name;
};

}
