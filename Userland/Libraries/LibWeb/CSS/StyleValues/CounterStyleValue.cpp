/*
 * Copyright (c) 2018-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleValue.h"
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Keyword.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

CounterStyleValue::CounterStyleValue(CounterFunction function, FlyString counter_name, ValueComparingNonnullRefPtr<CSSStyleValue const> counter_style, FlyString join_string)
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

// https://drafts.csswg.org/css-counter-styles-3/#generate-a-counter
static String generate_a_counter_representation(CSSStyleValue const& counter_style, i32 value)
{
    // When asked to generate a counter representation using a particular counter style for a particular
    // counter value, follow these steps:
    // TODO: 1. If the counter style is unknown, exit this algorithm and instead generate a counter representation
    //    using the decimal style and the same counter value.
    // TODO: 2. If the counter value is outside the range of the counter style, exit this algorithm and instead
    //    generate a counter representation using the counter style’s fallback style and the same counter value.
    // TODO: 3. Using the counter value and the counter algorithm for the counter style, generate an initial
    //    representation for the counter value.
    //    If the counter value is negative and the counter style uses a negative sign, instead generate an
    //    initial representation using the absolute value of the counter value.
    // TODO: 4. Prepend symbols to the representation as specified in the pad descriptor.
    // TODO: 5. If the counter value is negative and the counter style uses a negative sign, wrap the representation
    //    in the counter style’s negative sign as specified in the negative descriptor.
    // TODO: 6. Return the representation.

    // FIXME: Below is an ad-hoc implementation until we support @counter-style.
    //  It's based largely on the ListItemMarkerBox code, with minimal adjustments.
    if (counter_style.is_custom_ident()) {
        auto counter_style_name = counter_style.as_custom_ident().custom_ident();
        auto keyword = keyword_from_string(counter_style_name);
        if (keyword.has_value()) {
            auto list_style_type = keyword_to_list_style_type(*keyword);
            if (list_style_type.has_value()) {
                switch (*list_style_type) {
                case ListStyleType::Square:
                    return "▪"_string;
                case ListStyleType::Circle:
                    return "◦"_string;
                case ListStyleType::Disc:
                    return "•"_string;
                case ListStyleType::DisclosureClosed:
                    return "▸"_string;
                case ListStyleType::DisclosureOpen:
                    return "▾"_string;
                case ListStyleType::Decimal:
                    return MUST(String::formatted("{}", value));
                case ListStyleType::DecimalLeadingZero:
                    // This is weird, but in accordance to spec.
                    if (value < 10)
                        return MUST(String::formatted("0{}", value));
                    return MUST(String::formatted("{}", value));
                case ListStyleType::LowerAlpha:
                case ListStyleType::LowerLatin:
                    return MUST(String::from_byte_string(ByteString::bijective_base_from(value - 1).to_lowercase()));
                case ListStyleType::UpperAlpha:
                case ListStyleType::UpperLatin:
                    return MUST(String::from_byte_string(ByteString::bijective_base_from(value - 1)));
                case ListStyleType::LowerRoman:
                    return MUST(String::from_byte_string(ByteString::roman_number_from(value).to_lowercase()));
                case ListStyleType::UpperRoman:
                    return MUST(String::from_byte_string(ByteString::roman_number_from(value)));
                default:
                    break;
                }
            }
        }
    }
    // FIXME: Handle `symbols()` function for counter_style.

    dbgln("FIXME: Unsupported counter style '{}'", counter_style.to_string());
    return MUST(String::formatted("{}", value));
}

String CounterStyleValue::resolve(DOM::Element& element) const
{
    // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
    // one is first instantiated with a starting value of 0."
    auto& counters_set = element.ensure_counters_set();
    if (!counters_set.last_counter_with_name(m_properties.counter_name).has_value())
        counters_set.instantiate_a_counter(m_properties.counter_name, element.unique_id(), false, 0);

    // counter( <counter-name>, <counter-style>? )
    // "Represents the value of the innermost counter in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>."
    if (m_properties.function == CounterFunction::Counter) {
        // NOTE: This should always be present because of the handling of a missing counter above.
        auto& counter = counters_set.last_counter_with_name(m_properties.counter_name).value();
        return generate_a_counter_representation(m_properties.counter_style, counter.value.value_or(0).value());
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

        auto counter_string = generate_a_counter_representation(m_properties.counter_style, counter.value.value_or(0).value());
        if (!stb.is_empty())
            stb.append(m_properties.join_string);
        stb.append(counter_string);
    }
    return stb.to_string_without_validation();
}

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
    Vector<RefPtr<CSSStyleValue const>> list;
    list.append(CustomIdentStyleValue::create(m_properties.counter_name));
    if (m_properties.function == CounterFunction::Counters)
        list.append(StringStyleValue::create(m_properties.join_string.to_string()));
    if (m_properties.counter_style->to_keyword() != Keyword::Decimal)
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
