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

    ValueComparingNonnullRefPtr<StyleValue const> length_percentage() const { return wrap_rust_child(m_value->text_indent.length_percentage); }
    bool hanging() const { return m_value->text_indent.hanging; }
    bool each_line() const { return m_value->text_indent.each_line; }

    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

private:
    friend class StyleValue;

    explicit TextIndentStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::TextIndent, data)
    {
    }

    TextIndentStyleValue(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line);
};

}
