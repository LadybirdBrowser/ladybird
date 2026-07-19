/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class TextUnderlinePositionStyleValue : public StyleValueWithDefaultOperators<TextUnderlinePositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TextUnderlinePositionStyleValue const> create(TextUnderlinePositionHorizontal horizontal, TextUnderlinePositionVertical vertical)
    {
        return adopt_ref(*new (nothrow) TextUnderlinePositionStyleValue(horizontal, vertical));
    }
    virtual ~TextUnderlinePositionStyleValue() override = default;

    TextUnderlinePositionHorizontal horizontal() const { return static_cast<TextUnderlinePositionHorizontal>(m_value->text_underline_position.horizontal); }
    TextUnderlinePositionVertical vertical() const { return static_cast<TextUnderlinePositionVertical>(m_value->text_underline_position.vertical); }

    void serialize(StringBuilder&, SerializationMode) const;

    bool properties_equal(TextUnderlinePositionStyleValue const& other) const { return horizontal() == other.horizontal() && vertical() == other.vertical(); }

private:
    explicit TextUnderlinePositionStyleValue(TextUnderlinePositionHorizontal horizontal, TextUnderlinePositionVertical vertical)
        : StyleValueWithDefaultOperators(Type::TextUnderlinePosition, StyleValueFFI::rust_style_value_create_text_underline_position(to_underlying(horizontal), to_underlying(vertical)))
    {
    }
};

}
