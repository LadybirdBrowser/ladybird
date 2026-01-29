/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/SystemColor.h>

namespace Web::CSS::SystemColor {

Color accent_color(PreferredColorScheme)
{
    return Color(61, 174, 233);
}

Color accent_color_text(PreferredColorScheme)
{
    return Color(0, 0, 0);
}

Color active_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(213, 97, 82);
    }
    return Color(255, 0, 0);
}

Color button_border(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(72, 72, 72);
    }
    return Color(128, 128, 128);
}

Color button_face(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(32, 29, 25);
    }
    return Color(212, 208, 200);
}

Color button_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(235, 235, 235);
    }
    return Color(0, 0, 0);
}

Color canvas(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(20, 20, 20);
    }
    return Color(255, 255, 255);
}

Color canvas_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(235, 235, 235);
    }
    return Color(0, 0, 0);
}

Color field(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(32, 29, 25);
    }
    return Color(255, 255, 255);
}

Color field_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(235, 235, 235);
    }
    return Color(0, 0, 0);
}

Color gray_text(PreferredColorScheme)
{
    return Color(128, 128, 128);
}

Color highlight(PreferredColorScheme)
{
    return Color(61, 174, 233, 128);
}

Color highlight_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(20, 20, 20);
    }
    return Color(255, 255, 255);
}

Color link_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(100, 149, 237);
    }
    return Color(0, 0, 238);
}

Color mark(PreferredColorScheme)
{
    return Color(255, 255, 0);
}

Color mark_text(PreferredColorScheme)
{
    return Color(0, 0, 0);
}

Color selected_item(PreferredColorScheme)
{
    return Color(61, 174, 233);
}

Color selected_item_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(20, 20, 20);
    }
    return Color(255, 255, 255);
}

Color visited_text(PreferredColorScheme scheme)
{
    if (scheme == PreferredColorScheme::Dark) {
        return Color(156, 113, 212);
    }
    return Color(85, 26, 139);
}

}
