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
    virtual ~TextUnderlinePositionStyleValue() override = default;

    TextUnderlinePositionHorizontal horizontal() const { return static_cast<TextUnderlinePositionHorizontal>(m_value->text_underline_position.horizontal); }
    TextUnderlinePositionVertical vertical() const { return static_cast<TextUnderlinePositionVertical>(m_value->text_underline_position.vertical); }

private:
    friend class StyleValue;

    explicit TextUnderlinePositionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::TextUnderlinePosition, data)
    {
    }
};

}
