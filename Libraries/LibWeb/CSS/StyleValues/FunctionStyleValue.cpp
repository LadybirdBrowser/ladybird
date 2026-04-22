/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FunctionStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> FunctionStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_value = m_value->absolutized(context);

    if (absolutized_value == m_value)
        return *this;

    return FunctionStyleValue::create(m_name, absolutized_value);
}

void FunctionStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append(m_name);
    builder.append('(');
    m_value->serialize(builder, mode);
    builder.append(')');
}

}
