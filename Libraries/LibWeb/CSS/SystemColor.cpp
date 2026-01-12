/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/SystemColor.h>

namespace Web::CSS::SystemColor {

Gfx::Color accent_color(PreferredColorScheme)
{
    return Gfx::Color(61, 174, 233);
}

Gfx::Color accent_color_text(PreferredColorScheme)
{
    return Gfx::Color(255, 255, 255);
}

Gfx::Color active_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(213, 97, 82);
    }
    return Gfx::Color(255, 0, 0);
}

Gfx::Color button_border(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(72, 72, 72);
    }
    return Gfx::Color(128, 128, 128);
}

Gfx::Color button_face(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(32, 29, 25);
    }
    return Gfx::Color(212, 208, 200);
}

Gfx::Color button_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(235, 235, 235);
    }
    return Gfx::Color(0, 0, 0);
}

Gfx::Color canvas(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(20, 20, 20);
    }
    return Gfx::Color(255, 255, 255);
}

Gfx::Color canvas_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(235, 235, 235);
    }
    return Gfx::Color(0, 0, 0);
}

Gfx::Color field(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(32, 29, 25);
    }
    return Gfx::Color(255, 255, 255);
}

Gfx::Color field_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(235, 235, 235);
    }
    return Gfx::Color(0, 0, 0);
}

Gfx::Color gray_text(PreferredColorScheme)
{
    return Gfx::Color(128, 128, 128);
}

Gfx::Color highlight(PreferredColorScheme)
{
    return Gfx::Color(61, 174, 233);
}

Gfx::Color highlight_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(20, 20, 20);
    }
    return Gfx::Color(255, 255, 255);
}

Gfx::Color link_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(100, 149, 237);
    }
    return Gfx::Color(0, 0, 238);
}

Gfx::Color mark(PreferredColorScheme)
{
    return Gfx::Color(255, 255, 0);
}

Gfx::Color mark_text(PreferredColorScheme)
{
    return Gfx::Color(0, 0, 0);
}

Gfx::Color selected_item(PreferredColorScheme)
{
    return Gfx::Color(61, 174, 233);
}

Gfx::Color selected_item_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(20, 20, 20);
    }
    return Gfx::Color(255, 255, 255);
}

Gfx::Color visited_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Gfx::Color(156, 113, 212);
    }
    return Gfx::Color(85, 26, 139);
}

}
