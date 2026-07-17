/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2024-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
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

static StyleValueFFI::StyleValueData* make_counter_data(CounterStyleValue::CounterFunction function, Utf16FlyString const& counter_name, ValueComparingNonnullRefPtr<StyleValue const> const& counter_style, Utf16FlyString const& join_string)
{
    // The Rust allocation takes ownership of one strong reference to the counter style.
    counter_style->ref();
    return StyleValueFFI::rust_style_value_create_counter(to_underlying(function), counter_name.to_raw_leaked(), counter_style.ptr(), join_string.to_raw_leaked());
}

CounterStyleValue::CounterStyleValue(CounterFunction function, Utf16FlyString counter_name, ValueComparingNonnullRefPtr<StyleValue const> counter_style, Utf16FlyString join_string)
    : StyleValueWithDefaultOperators(Type::Counter, make_counter_data(function, counter_name, counter_style, join_string))
{
}

CounterStyleValue::~CounterStyleValue() = default;

Utf16String CounterStyleValue::resolve(DOM::AbstractElement& element_reference) const
{
    // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
    // one is first instantiated with a starting value of 0."
    auto& counters_set = element_reference.ensure_counters_set();
    if (!counters_set.last_counter_with_name(counter_name()).has_value())
        counters_set.instantiate_a_counter(counter_name(), element_reference, false, 0);

    // counter( <counter-name>, <counter-style>? )
    // "Represents the value of the innermost counter in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>."
    if (function_type() == CounterFunction::Counter) {
        // NOTE: This should always be present because of the handling of a missing counter above.
        auto& counter = counters_set.last_counter_with_name(counter_name()).value();
        auto const& style_scope = element_reference.style_scope();
        return generate_a_counter_representation(counter_style()->as_counter_style().resolve_counter_style(style_scope), style_scope, counter.value.value_or(0));
    }

    // counters( <counter-name>, <string>, <counter-style>? )
    // "Represents the values of all the counters in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>, sorted in outermost-first to innermost-last order
    // and joined by the specified <string>."
    // NOTE: The way counters sets are inherited, this should be the order they appear in the counters set.
    Utf16StringBuilder stb;
    for (auto const& counter : counters_set.counters()) {
        if (counter.name != counter_name())
            continue;

        auto const& style_scope = element_reference.style_scope();
        auto counter_string = generate_a_counter_representation(counter_style()->as_counter_style().resolve_counter_style(style_scope), style_scope, counter.value.value_or(0));
        if (!stb.is_empty())
            stb.append(join_string().view());
        stb.append(counter_string);
    }
    return stb.to_string();
}

// https://drafts.csswg.org/cssom-1/#ref-for-typedef-counter
void CounterStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    // The return value of the following algorithm:
    // 1. Let s be the empty string.
    // (We use builder instead)

    // 2. If <counter> has three CSS component values append the string "counters(" to s.
    if (function_type() == CounterFunction::Counters)
        builder.append("counters("sv);

    // 3. If <counter> has two CSS component values append the string "counter(" to s.
    else if (function_type() == CounterFunction::Counter)
        builder.append("counter("sv);

    // 4. Let list be a list of CSS component values belonging to <counter>,
    //    omitting the last CSS component value if it is "decimal".
    Vector<RefPtr<StyleValue const>> list;
    list.append(CustomIdentStyleValue::create(counter_name()));
    if (function_type() == CounterFunction::Counters)
        list.append(StringStyleValue::create(join_string()));
    if (counter_style()->to_string(mode) != "decimal"sv)
        list.append(counter_style());

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
    return function_type() == other.function_type() && counter_name() == other.counter_name()
        && counter_style() == other.counter_style() && join_string() == other.join_string();
}

}
