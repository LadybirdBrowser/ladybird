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

RefPtr<CounterStyle const> CounterStyleStyleValue::resolve_counter_style(HashMap<FlyString, NonnullRefPtr<CounterStyle const>> const& registered_counter_styles) const
{
    // FIXME: Support symbols() function for anonymous counter style
    return registered_counter_styles.get(m_name).value_or(nullptr);
}

}
