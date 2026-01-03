/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AddFunctionStyleValue.h"

namespace Web::CSS {

void AddFunctionStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("add("sv);
    m_value->serialize(builder, mode);
    builder.append(')');
}

}
