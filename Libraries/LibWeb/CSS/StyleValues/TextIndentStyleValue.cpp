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
    : StyleValueWithDefaultOperators(Type::TextIndent)
    , m_value(StyleValueFFI::rust_style_value_create_text_indent(&length_percentage.leak_ref(), hanging == Hanging::Yes, each_line == EachLine::Yes))
{
}

TextIndentStyleValue::~TextIndentStyleValue() = default;

void TextIndentStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    length_percentage().serialize(builder, mode);
    if (each_line())
        builder.append(" each-line"sv);
    if (hanging())
        builder.append(" hanging"sv);
}

ValueComparingNonnullRefPtr<StyleValue const> TextIndentStyleValue::absolutized(ComputationContext const& context) const
{
    auto new_length_percentage = length_percentage().absolutized(context);
    if (new_length_percentage->equals(length_percentage()))
        return *this;
    return create(move(new_length_percentage),
        hanging() ? Hanging::Yes : Hanging::No,
        each_line() ? EachLine::Yes : EachLine::No);
}

bool TextIndentStyleValue::properties_equal(TextIndentStyleValue const& other) const
{
    return length_percentage() == other.length_percentage()
        && each_line() == other.each_line()
        && hanging() == other.hanging();
}

}
