/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-lists-3/#counter-functions
class CounterStyleValue : public StyleValueWithDefaultOperators<CounterStyleValue> {
public:
    enum class CounterFunction {
        Counter,
        Counters,
    };

    static ValueComparingNonnullRefPtr<CounterStyleValue> create_counter(FlyString counter_name, ValueComparingNonnullRefPtr<CSSStyleValue const> counter_style)
    {
        return adopt_ref(*new (nothrow) CounterStyleValue(CounterFunction::Counter, move(counter_name), move(counter_style), {}));
    }
    static ValueComparingNonnullRefPtr<CounterStyleValue> create_counters(FlyString counter_name, FlyString join_string, ValueComparingNonnullRefPtr<CSSStyleValue const> counter_style)
    {
        return adopt_ref(*new (nothrow) CounterStyleValue(CounterFunction::Counters, move(counter_name), move(counter_style), move(join_string)));
    }
    virtual ~CounterStyleValue() override;

    CounterFunction function_type() const { return m_properties.function; }
    auto counter_name() const { return m_properties.counter_name; }
    auto counter_style() const { return m_properties.counter_style; }
    auto join_string() const { return m_properties.join_string; }

    String resolve(DOM::Element&) const;

    virtual String to_string() const override;

    bool properties_equal(CounterStyleValue const& other) const;

private:
    explicit CounterStyleValue(CounterFunction, FlyString counter_name, ValueComparingNonnullRefPtr<CSSStyleValue const> counter_style, FlyString join_string);

    struct Properties {
        CounterFunction function;
        FlyString counter_name;
        ValueComparingNonnullRefPtr<CSSStyleValue const> counter_style;
        FlyString join_string;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
