/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ColorSchemeStyleValue final : public StyleValueWithDefaultOperators<ColorSchemeStyleValue> {
public:
    virtual ~ColorSchemeStyleValue() override = default;

    Vector<Utf16FlyString> schemes() const
    {
        auto const& list = m_value->color_scheme.schemes;
        Vector<Utf16FlyString> schemes;
        schemes.ensure_capacity(list.length);
        for (size_t i = 0; i < list.length; ++i)
            schemes.unchecked_append(Utf16FlyString::from_raw(list.pointer[i].raw));
        return schemes;
    }
    bool only() const { return m_value->color_scheme.only; }

private:
    friend class StyleValue;

    explicit ColorSchemeStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::ColorScheme, data)
    {
    }
};

}
