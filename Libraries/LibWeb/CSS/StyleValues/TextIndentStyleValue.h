/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class TextIndentStyleValue : public StyleValueWithDefaultOperators<TextIndentStyleValue> {
public:
    enum class Hanging : u8 {
        No,
        Yes,
    };
    enum class EachLine : u8 {
        No,
        Yes,
    };

    static ValueComparingNonnullRefPtr<TextIndentStyleValue const> create(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line);
    virtual ~TextIndentStyleValue() override;

    StyleValue const& length_percentage() const { return *static_cast<StyleValue const*>(m_value->text_indent.length_percentage.pointer); }
    bool hanging() const { return m_value->text_indent.hanging; }
    bool each_line() const { return m_value->text_indent.each_line; }

    void serialize(StringBuilder&, SerializationMode) const;
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;
    bool properties_equal(TextIndentStyleValue const&) const;

private:
    TextIndentStyleValue(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line);
};

}
