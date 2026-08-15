/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TextIndentStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<TextIndentStyleValue const> TextIndentStyleValue::create(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line)
{
    return adopt_ref(*new (nothrow) TextIndentStyleValue(move(length_percentage), hanging, each_line));
}

TextIndentStyleValue::TextIndentStyleValue(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line)
    : StyleValueWithDefaultOperators(Type::TextIndent, StyleValueFFI::rust_style_value_create_text_indent(StyleValueFFI::rust_style_value_retain(length_percentage->rust_style_value_data()), hanging == Hanging::Yes, each_line == EachLine::Yes))
{
}

TextIndentStyleValue::~TextIndentStyleValue() = default;

ValueComparingNonnullRefPtr<StyleValue const> TextIndentStyleValue::absolutized(ComputationContext const& context) const
{
    auto new_length_percentage = length_percentage()->absolutized(context);
    if (new_length_percentage->equals(length_percentage()))
        return *this;
    return create(move(new_length_percentage),
        hanging() ? Hanging::Yes : Hanging::No,
        each_line() ? EachLine::Yes : EachLine::No);
}

}
