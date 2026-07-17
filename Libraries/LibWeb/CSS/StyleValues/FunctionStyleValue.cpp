/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FunctionStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<StyleValue const> FunctionStyleValue::absolutized(ComputationContext const& context) const
{
    auto absolutized_value = value()->absolutized(context);

    if (absolutized_value == value())
        return *this;

    return FunctionStyleValue::create(name(), absolutized_value);
}

void FunctionStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append(name().view());
    builder.append('(');
    value()->serialize(builder, mode);
    builder.append(')');
}

}
