/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>

namespace Web::CSS {

UnicodeRangeStyleValue::UnicodeRangeStyleValue(Gfx::UnicodeRange unicode_range)
    : StyleValueWithDefaultOperators(Type::UnicodeRange, StyleValueFFI::rust_style_value_create_unicode_range(unicode_range.min_code_point(), unicode_range.max_code_point()))
{
}

UnicodeRangeStyleValue::~UnicodeRangeStyleValue() = default;

}
