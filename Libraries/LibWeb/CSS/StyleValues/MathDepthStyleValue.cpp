/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "MathDepthStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<MathDepthStyleValue> MathDepthStyleValue::create_auto_add()
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::AutoAdd));
}

ValueComparingNonnullRefPtr<MathDepthStyleValue> MathDepthStyleValue::create_add(ValueComparingNonnullRefPtr<CSSStyleValue const> integer_value)
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::Add, move(integer_value)));
}

ValueComparingNonnullRefPtr<MathDepthStyleValue> MathDepthStyleValue::create_integer(ValueComparingNonnullRefPtr<CSSStyleValue const> integer_value)
{
    return adopt_ref(*new (nothrow) MathDepthStyleValue(MathDepthType::Integer, move(integer_value)));
}

MathDepthStyleValue::MathDepthStyleValue(MathDepthType type, ValueComparingRefPtr<CSSStyleValue const> integer_value)
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

String MathDepthStyleValue::to_string() const
{
    switch (m_type) {
    case MathDepthType::AutoAdd:
        return "auto-add"_string;
    case MathDepthType::Add:
        return MUST(String::formatted("add({})", m_integer_value->to_string()));
    case MathDepthType::Integer:
        return m_integer_value->to_string();
    }
    VERIFY_NOT_REACHED();
}

}
