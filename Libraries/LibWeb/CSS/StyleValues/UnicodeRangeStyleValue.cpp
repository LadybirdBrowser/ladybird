/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>

namespace Web::CSS {

UnicodeRangeStyleValue::UnicodeRangeStyleValue(Gfx::UnicodeRange unicode_range)
    : StyleValueWithDefaultOperators(Type::UnicodeRange)
    , m_unicode_range(unicode_range)
{
}

UnicodeRangeStyleValue::~UnicodeRangeStyleValue() = default;

String UnicodeRangeStyleValue::to_string(SerializationMode) const
{
    return m_unicode_range.to_string();
}

bool UnicodeRangeStyleValue::properties_equal(UnicodeRangeStyleValue const& other) const
{
    return m_unicode_range == other.m_unicode_range;
}

}
