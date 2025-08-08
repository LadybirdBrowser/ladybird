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

String MathDepthStyleValue::to_string(SerializationMode mode) const
{
    switch (m_type) {
    case MathDepthType::AutoAdd:
        return "auto-add"_string;
    case MathDepthType::Add:
        return MUST(String::formatted("add({})", m_integer_value->to_string(mode)));
    case MathDepthType::Integer:
        return m_integer_value->to_string(mode);
    }
    VERIFY_NOT_REACHED();
}

}
