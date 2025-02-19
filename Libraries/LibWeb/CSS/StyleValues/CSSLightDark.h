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

    static ValueComparingNonnullRefPtr<CSSLightDark> create(ValueComparingNonnullRefPtr<CSSStyleValue> light, ValueComparingNonnullRefPtr<CSSStyleValue> dark)
    {
        return AK::adopt_ref(*new (nothrow) CSSLightDark(move(light), move(dark)));
    }

    virtual bool equals(CSSStyleValue const&) const override;
    virtual Color to_color(Optional<Layout::NodeWithStyle const&>) const override;
    virtual String to_string(SerializationMode) const override;

private:
    CSSLightDark(ValueComparingNonnullRefPtr<CSSStyleValue> light, ValueComparingNonnullRefPtr<CSSStyleValue> dark)
        : CSSColorValue(CSSColorValue::ColorType::LightDark, ColorSyntax::Modern)
        , m_properties { .light = move(light), .dark = move(dark) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue> light;
        ValueComparingNonnullRefPtr<CSSStyleValue> dark;
        bool operator==(Properties const&) const = default;
    };

    Properties m_properties;
};

} // Web::CSS
