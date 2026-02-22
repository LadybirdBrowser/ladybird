/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyleStyleValue.h"
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

void CounterStyleStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(m_name);
}

Optional<CounterStyleNameKeyword> CounterStyleStyleValue::to_counter_style_name_keyword() const
{
    return keyword_from_string(m_name)
        .map([](auto keyword) { return keyword_to_counter_style_name_keyword(keyword); })
        .value_or(OptionalNone {});
}

Optional<CounterStyle const&> CounterStyleStyleValue::resolve_counter_style(HashMap<FlyString, CounterStyle> const& registered_counter_styles) const
{
    // FIXME: Support symbols() function for anonymous counter style
    return registered_counter_styles.get(m_name);
}

}
