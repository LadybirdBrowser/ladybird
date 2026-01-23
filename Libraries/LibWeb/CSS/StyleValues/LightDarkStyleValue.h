/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-color-5/#funcdef-light-dark
class LightDarkStyleValue final : public ColorStyleValue {
public:
    virtual ~LightDarkStyleValue() override = default;

    static ValueComparingNonnullRefPtr<LightDarkStyleValue const> create(ValueComparingNonnullRefPtr<StyleValue const> light, ValueComparingNonnullRefPtr<StyleValue const> dark)
    {
        return AK::adopt_ref(*new (nothrow) LightDarkStyleValue(move(light), move(dark)));
    }

    virtual bool equals(StyleValue const&) const override;
    virtual Optional<Color> to_color(ColorResolutionContext) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

private:
    LightDarkStyleValue(ValueComparingNonnullRefPtr<StyleValue const> light, ValueComparingNonnullRefPtr<StyleValue const> dark)
        : ColorStyleValue(ColorStyleValue::ColorType::LightDark, ColorSyntax::Modern)
        , m_properties { .light = move(light), .dark = move(dark) }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<StyleValue const> light;
        ValueComparingNonnullRefPtr<StyleValue const> dark;
        bool operator==(Properties const&) const = default;
    };

    Properties m_properties;
};

} // Web::CSS
