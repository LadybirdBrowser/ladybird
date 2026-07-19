/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-lists-3/#counter-functions
class CounterStyleValue : public StyleValueWithDefaultOperators<CounterStyleValue> {
public:
    enum class CounterFunction {
        Counter,
        Counters,
    };

    static ValueComparingNonnullRefPtr<CounterStyleValue const> create_counter(Utf16FlyString counter_name, ValueComparingNonnullRefPtr<StyleValue const> counter_style)
    {
        return adopt_ref(*new (nothrow) CounterStyleValue(CounterFunction::Counter, move(counter_name), move(counter_style), {}));
    }
    static ValueComparingNonnullRefPtr<CounterStyleValue const> create_counters(Utf16FlyString counter_name, Utf16FlyString join_string, ValueComparingNonnullRefPtr<StyleValue const> counter_style)
    {
        return adopt_ref(*new (nothrow) CounterStyleValue(CounterFunction::Counters, move(counter_name), move(counter_style), move(join_string)));
    }
    virtual ~CounterStyleValue() override;

    CounterFunction function_type() const { return static_cast<CounterFunction>(m_value->counter.function); }
    Utf16FlyString counter_name() const { return Utf16FlyString::from_raw(m_value->counter.counter_name.raw); }
    ValueComparingNonnullRefPtr<StyleValue const> counter_style() const { return *static_cast<StyleValue const*>(m_value->counter.counter_style.pointer); }
    Utf16FlyString join_string() const { return Utf16FlyString::from_raw(m_value->counter.join_string.raw); }

    Utf16String resolve(DOM::AbstractElement&) const;

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(CounterStyleValue const& other) const;

private:
    explicit CounterStyleValue(CounterFunction, Utf16FlyString counter_name, ValueComparingNonnullRefPtr<StyleValue const> counter_style, Utf16FlyString join_string);
};

}
