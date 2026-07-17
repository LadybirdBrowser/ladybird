/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>

namespace Web::CSS {

UnicodeRangeStyleValue::UnicodeRangeStyleValue(Gfx::UnicodeRange unicode_range)
    : StyleValueWithDefaultOperators(Type::UnicodeRange)
    , m_value(StyleValueFFI::rust_style_value_create_unicode_range(unicode_range.min_code_point(), unicode_range.max_code_point()))
{
}

UnicodeRangeStyleValue::~UnicodeRangeStyleValue() = default;

void UnicodeRangeStyleValue::serialize(StringBuilder& builder, SerializationMode) const
{
    builder.append(unicode_range().to_string());
}

bool UnicodeRangeStyleValue::properties_equal(UnicodeRangeStyleValue const& other) const
{
    return unicode_range() == other.unicode_range();
}

}
