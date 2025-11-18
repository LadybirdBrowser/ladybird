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

    StyleValue const& length_percentage() const { return m_length_percentage; }
    bool hanging() const { return m_hanging; }
    bool each_line() const { return m_each_line; }

    virtual String to_string(SerializationMode) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    bool properties_equal(TextIndentStyleValue const&) const;

private:
    TextIndentStyleValue(NonnullRefPtr<StyleValue const> length_percentage, Hanging hanging, EachLine each_line);

    NonnullRefPtr<StyleValue const> m_length_percentage;
    bool m_hanging;
    bool m_each_line;
};

}
