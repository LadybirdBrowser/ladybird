/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleValue.h"
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>

namespace Web::CSS {

CounterStyleValue::CounterStyleValue(CounterFunction function, FlyString counter_name, ValueComparingNonnullRefPtr<StyleValue const> counter_style, FlyString join_string)
    : StyleValueWithDefaultOperators(Type::Counter)
    , m_properties {
        .function = function,
        .counter_name = move(counter_name),
        .counter_style = move(counter_style),
        .join_string = move(join_string)
    }
{
}

CounterStyleValue::~CounterStyleValue() = default;

// https://drafts.csswg.org/cssom-1/#ref-for-typedef-counter
String CounterStyleValue::to_string() const
{
    // The return value of the following algorithm:
    // 1. Let s be the empty string.
    StringBuilder s;

    // 2. If <counter> has three CSS component values append the string "counters(" to s.
    if (m_properties.function == CounterFunction::Counters)
        s.append("counters("sv);

    // 3. If <counter> has two CSS component values append the string "counter(" to s.
    else if (m_properties.function == CounterFunction::Counter)
        s.append("counter("sv);

    // 4. Let list be a list of CSS component values belonging to <counter>,
    //    omitting the last CSS component value if it is "decimal".
    Vector<RefPtr<StyleValue const>> list;
    list.append(CustomIdentStyleValue::create(m_properties.counter_name));
    if (m_properties.function == CounterFunction::Counters)
        list.append(StringStyleValue::create(m_properties.join_string.to_string()));
    if (m_properties.counter_style->to_identifier() != ValueID::Decimal)
        list.append(m_properties.counter_style);

    // 5. Let each item in list be the result of invoking serialize a CSS component value on that item.
    // 6. Append the result of invoking serialize a comma-separated list on list to s.
    serialize_a_comma_separated_list(s, list, [](auto& builder, auto& item) {
        builder.append(item->to_string());
    });

    // 7. Append ")" (U+0029) to s.
    s.append(")"sv);

    // 8. Return s.
    return MUST(s.to_string());
}

bool CounterStyleValue::properties_equal(CounterStyleValue const& other) const
{
    return m_properties == other.m_properties;
}

}
