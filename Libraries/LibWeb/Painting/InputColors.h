/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibGfx/Palette.h>
#include <LibWeb/CSS/SystemColor.h>

namespace Web::Painting {

// Note: the color names reflect what the colors would be for a light theme,
// not necessary the actual colors.
struct InputColors {
    Color accent;
    Color base;
    Color dark_gray;
    Color gray;
    Color mid_gray;
    Color light_gray;

    Color background_color(bool enabled) { return enabled ? base : light_gray; }
    Color border_color(bool enabled) { return enabled ? gray : mid_gray; }

    static Color get_shade(Color color, float amount, CSS::PreferredColorScheme color_scheme)
    {
        auto base_color = CSS::SystemColor::canvas(color_scheme);
        return color.mixed_with(base_color, amount);
    }
};

static InputColors compute_input_colors(CSS::PreferredColorScheme color_scheme, Optional<Color> accent_color)
{
    // These shades have been picked to work well for all themes and have enough variation to paint
    // all input states (disabled, enabled, checked, etc).
    auto base_text_color = CSS::SystemColor::canvas_text(color_scheme);
    auto accent = accent_color.value_or(CSS::SystemColor::accent_color(color_scheme));
    auto base = InputColors::get_shade(base_text_color.inverted(), 0.8f, color_scheme);
    auto dark_gray = InputColors::get_shade(base_text_color, 0.3f, color_scheme);
    auto gray = InputColors::get_shade(dark_gray, 0.4f, color_scheme);
    auto mid_gray = InputColors::get_shade(gray, 0.3f, color_scheme);
    auto light_gray = InputColors::get_shade(mid_gray, 0.3f, color_scheme);
    return InputColors {
        .accent = accent,
        .base = base,
        .dark_gray = dark_gray,
        .gray = gray,
        .mid_gray = mid_gray,
        .light_gray = light_gray
    };
}

}
