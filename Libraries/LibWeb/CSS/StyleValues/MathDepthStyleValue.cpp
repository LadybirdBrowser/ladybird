/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MathDepthStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<MathDepthStyleValue const> MathDepthStyleValue::create_auto_add()
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::AutoAdd));
}

ValueComparingNonnullRefPtr<MathDepthStyleValue const> MathDepthStyleValue::create_add(ValueComparingNonnullRefPtr<StyleValue const> integer_value)
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::Add, move(integer_value)));
}

ValueComparingNonnullRefPtr<MathDepthStyleValue const> MathDepthStyleValue::create_integer(ValueComparingNonnullRefPtr<StyleValue const> integer_value)
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::Integer, move(integer_value)));
}

MathDepthStyleValue::MathDepthStyleValue(MathDepthType type, ValueComparingRefPtr<StyleValue const> integer_value)
    : StyleValueWithDefaultOperators(Type::MathDepth)
    , m_type(type)
    , m_integer_value(move(integer_value))
{
}

bool MathDepthStyleValue::properties_equal(MathDepthStyleValue const& other) const
{
    return m_type == other.m_type
        && m_integer_value == other.m_integer_value;
}

void MathDepthStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    switch (m_type) {
    case MathDepthType::AutoAdd:
        builder.append("auto-add"sv);
        break;
    case MathDepthType::Add:
        builder.append("add("sv);
        m_integer_value->serialize(builder, mode);
        builder.append(')');
        break;
    case MathDepthType::Integer:
        m_integer_value->serialize(builder, mode);
        break;
    }
}

}
