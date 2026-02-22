/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CountersSet.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

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

String CounterStyleValue::resolve(DOM::AbstractElement& element_reference) const
{
    // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
    // one is first instantiated with a starting value of 0."
    auto& counters_set = element_reference.ensure_counters_set();
    if (!counters_set.last_counter_with_name(m_properties.counter_name).has_value())
        counters_set.instantiate_a_counter(m_properties.counter_name, element_reference, false, 0);

    auto const& registered_counter_styles = element_reference.document().registered_counter_styles();

    // counter( <counter-name>, <counter-style>? )
    // "Represents the value of the innermost counter in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>."
    if (m_properties.function == CounterFunction::Counter) {
        // NOTE: This should always be present because of the handling of a missing counter above.
        auto& counter = counters_set.last_counter_with_name(m_properties.counter_name).value();
        return generate_a_counter_representation(m_properties.counter_style->as_counter_style().resolve_counter_style(registered_counter_styles), registered_counter_styles, counter.value.value_or(0).value());
    }

    // counters( <counter-name>, <string>, <counter-style>? )
    // "Represents the values of all the counters in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>, sorted in outermost-first to innermost-last order
    // and joined by the specified <string>."
    // NOTE: The way counters sets are inherited, this should be the order they appear in the counters set.
    StringBuilder stb;
    for (auto const& counter : counters_set.counters()) {
        if (counter.name != m_properties.counter_name)
            continue;

        auto counter_string = generate_a_counter_representation(m_properties.counter_style->as_counter_style().resolve_counter_style(registered_counter_styles), registered_counter_styles, counter.value.value_or(0).value());
        if (!stb.is_empty())
            stb.append(m_properties.join_string);
        stb.append(counter_string);
    }
    return stb.to_string_without_validation();
}

// https://drafts.csswg.org/cssom-1/#ref-for-typedef-counter
void CounterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // The return value of the following algorithm:
    // 1. Let s be the empty string.
    // (We use builder instead)

    // 2. If <counter> has three CSS component values append the string "counters(" to s.
    if (m_properties.function == CounterFunction::Counters)
        builder.append("counters("sv);

    // 3. If <counter> has two CSS component values append the string "counter(" to s.
    else if (m_properties.function == CounterFunction::Counter)
        builder.append("counter("sv);

    // 4. Let list be a list of CSS component values belonging to <counter>,
    //    omitting the last CSS component value if it is "decimal".
    Vector<RefPtr<StyleValue const>> list;
    list.append(CustomIdentStyleValue::create(m_properties.counter_name));
    if (m_properties.function == CounterFunction::Counters)
        list.append(StringStyleValue::create(m_properties.join_string.to_string()));
    if (m_properties.counter_style->to_string(mode) != "decimal"sv)
        list.append(m_properties.counter_style);

    // 5. Let each item in list be the result of invoking serialize a CSS component value on that item.
    // 6. Append the result of invoking serialize a comma-separated list on list to s.
    serialize_a_comma_separated_list(builder, list, [mode](auto& b, auto& item) {
        item->serialize(b, mode);
    });

    // 7. Append ")" (U+0029) to s.
    builder.append(")"sv);
}

bool CounterStyleValue::properties_equal(CounterStyleValue const& other) const
{
    return m_properties == other.m_properties;
}

}
