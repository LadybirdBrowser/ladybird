/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AddFunctionStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> AddFunctionStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_value = m_value->absolutized(context);

    if (absolutized_value == m_value)
        return *this;

    return AddFunctionStyleValue::create(m_value->absolutized(context));
}

void AddFunctionStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("add("sv);
    m_value->serialize(builder, mode);
    builder.append(')');
}

}
