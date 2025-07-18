/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/CSSColorValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-color-5/#funcdef-light-dark
class CSSLightDark final : public CSSColorValue {
public:
    virtual ~CSSLightDark() override = default;

    static ValueComparingNonnullRefPtr<CSSLightDark const> create(ValueComparingNonnullRefPtr<CSSStyleValue const> light, ValueComparingNonnullRefPtr<CSSStyleValue const> dark)
    {
        return AK::adopt_ref(*new (nothrow) CSSLightDark(move(light), move(dark)));
    }

    virtual bool equals(CSSStyleValue const&) const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual String to_string(SerializationMode) const override;

private:
    CSSLightDark(ValueComparingNonnullRefPtr<CSSStyleValue const> light, ValueComparingNonnullRefPtr<CSSStyleValue const> dark)
        : CSSColorValue(CSSColorValue::ColorType::LightDark, ColorSyntax::Modern)
        , m_properties { .light = move(light), .dark = move(dark) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue const> light;
        ValueComparingNonnullRefPtr<CSSStyleValue const> dark;
        bool operator==(Properties const&) const = default;
    };

    Properties m_properties;
};

} // Web::CSS
