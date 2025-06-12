/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class ColorSchemeStyleValue final : public StyleValueWithDefaultOperators<ColorSchemeStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ColorSchemeStyleValue const> create(Vector<String> schemes, bool only)
    {
        return adopt_ref(*new (nothrow) ColorSchemeStyleValue(move(schemes), only));
    }
    static ValueComparingNonnullRefPtr<ColorSchemeStyleValue const> normal()
    {
        return adopt_ref(*new (nothrow) ColorSchemeStyleValue({}, false));
    }
    virtual ~ColorSchemeStyleValue() override = default;

    Vector<String> const& schemes() const { return m_properties.schemes; }
    bool const& only() const { return m_properties.only; }
    virtual String to_string(SerializationMode) const override;

    bool properties_equal(ColorSchemeStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    ColorSchemeStyleValue(Vector<String> schemes, bool only)
        : StyleValueWithDefaultOperators(Type::ColorScheme)
        , m_properties { .schemes = move(schemes), .only = only }
    {
    }

    struct Properties {
        Vector<String> schemes;
        bool only;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
