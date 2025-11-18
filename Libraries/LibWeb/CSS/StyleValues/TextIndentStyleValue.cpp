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
    , m_length_percentage(move(length_percentage))
    , m_hanging(hanging == Hanging::Yes)
    , m_each_line(each_line == EachLine::Yes)
{
}

TextIndentStyleValue::~TextIndentStyleValue() = default;

String TextIndentStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    builder.append(m_length_percentage->to_string(mode));
    if (m_each_line)
        builder.append(" each-line"sv);
    if (m_hanging)
        builder.append(" hanging"sv);
    return builder.to_string_without_validation();
}

ValueComparingNonnullRefPtr<StyleValue const> TextIndentStyleValue::absolutized(ComputationContext const& context) const
{
    auto new_length_percentage = m_length_percentage->absolutized(context);
    if (new_length_percentage->equals(m_length_percentage))
        return *this;
    return create(move(new_length_percentage),
        m_hanging ? Hanging::Yes : Hanging::No,
        m_each_line ? EachLine::Yes : EachLine::No);
}

bool TextIndentStyleValue::properties_equal(TextIndentStyleValue const& other) const
{
    return m_length_percentage == other.m_length_percentage
        && m_each_line == other.m_each_line
        && m_hanging == other.m_hanging;
}

}
