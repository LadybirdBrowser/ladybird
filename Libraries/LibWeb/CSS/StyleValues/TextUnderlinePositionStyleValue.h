/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class TextUnderlinePositionStyleValue : public StyleValueWithDefaultOperators<TextUnderlinePositionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<TextUnderlinePositionStyleValue const> create(TextUnderlinePositionHorizontal horizontal, TextUnderlinePositionVertical vertical)
    {
        return adopt_ref(*new (nothrow) TextUnderlinePositionStyleValue(horizontal, vertical));
    }
    virtual ~TextUnderlinePositionStyleValue() override = default;

    TextUnderlinePositionHorizontal horizontal() const { return m_horizontal; }
    TextUnderlinePositionVertical vertical() const { return m_vertical; }

    virtual String to_string(SerializationMode serialization_mode) const override;

    bool properties_equal(TextUnderlinePositionStyleValue const& other) const { return m_horizontal == other.m_horizontal && m_vertical == other.m_vertical; }

private:
    explicit TextUnderlinePositionStyleValue(TextUnderlinePositionHorizontal horizontal, TextUnderlinePositionVertical vertical)
        : StyleValueWithDefaultOperators(Type::TextUnderlinePosition)
        , m_horizontal(horizontal)
        , m_vertical(vertical)
    {
    }

    TextUnderlinePositionHorizontal m_horizontal;
    TextUnderlinePositionVertical m_vertical;
};

}
