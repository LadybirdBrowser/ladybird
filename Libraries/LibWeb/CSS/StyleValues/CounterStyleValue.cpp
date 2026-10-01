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

// The function discriminant crosses the style value FFI as a raw code; the Rust serializer
// depends on it.
static_assert(to_underlying(CounterStyleValue::CounterFunction::Counter) == 0);
static_assert(to_underlying(CounterStyleValue::CounterFunction::Counters) == 1);

// So does a symbols() function's type, which the Rust counter style resolution reads.
static_assert(to_underlying(SymbolsType::Cyclic) == 0);
static_assert(to_underlying(SymbolsType::Numeric) == 1);
static_assert(to_underlying(SymbolsType::Alphabetic) == 2);
static_assert(to_underlying(SymbolsType::Symbolic) == 3);
static_assert(to_underlying(SymbolsType::Fixed) == 4);

static StyleValueFFI::StyleValueData const* make_counter_data(CounterStyleValue::CounterFunction function, Utf16FlyString const& counter_name, ValueComparingNonnullRefPtr<StyleValue const> const& counter_style, Utf16FlyString const& join_string)
{
    // The Rust allocation takes ownership of one strong reference to the counter style data.
    return StyleValueFFI::rust_style_value_create_counter(to_underlying(function), counter_name.to_raw_leaked(), StyleValueFFI::rust_style_value_retain(counter_style->rust_style_value_data()), join_string.to_raw_leaked());
}

CounterStyleValue::CounterStyleValue(CounterFunction function, Utf16FlyString counter_name, ValueComparingNonnullRefPtr<StyleValue const> counter_style, Utf16FlyString join_string)
    : StyleValueWithDefaultOperators(Type::Counter, make_counter_data(function, counter_name, counter_style, join_string))
{
}

CounterStyleValue::~CounterStyleValue() = default;

}
