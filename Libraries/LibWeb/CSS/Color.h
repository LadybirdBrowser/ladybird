/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ValueComparingRefPtr.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

class Color {
public:
    explicit Color(Gfx::Color resolved_color, ValueComparingRefPtr<StyleValue const> style_value = {});

    Gfx::Color resolved() const;
    Gfx::SRGBA01 unpremultiplied_srgba() const { return m_srgba; }
    ValueComparingRefPtr<StyleValue const> style_value() const { return m_style_value; }

    void serialize(StringBuilder&, SerializationMode) const;
    String to_string(SerializationMode serialization_mode) const;

private:
    ValueComparingRefPtr<StyleValue const> m_style_value;
    Gfx::SRGBA01 m_srgba;
};

enum class ColorSpace : u8 {
    A98Rgb,
    DisplayP3,
    Hsl,
    Hwb,
    Lab,
    Lch,
    Oklab,
    Oklch,
    ProphotoRgb,
    Rec2020,
    Srgb,
    SrgbLinear,
    XyzD50,
    XyzD65,
};

}
