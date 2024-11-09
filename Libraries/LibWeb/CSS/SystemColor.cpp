/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/SystemColor.h>

namespace Web::CSS::SystemColor {

Color accent_color()
{
    return Color(61, 174, 233);
}

Color accent_color_text()
{
    return Color(255, 255, 255);
}

Color active_text()
{
    return Color(255, 0, 0);
}

Color button_border()
{
    return Color(128, 128, 128);
}

Color button_face()
{
    return Color(212, 208, 200);
}

Color button_text()
{
    return Color(0, 0, 0);
}

Color canvas()
{
    return Color(255, 255, 255);
}

Color canvas_text()
{
    return Color(0, 0, 0);
}

Color field()
{
    return Color(255, 255, 255);
}

Color field_text()
{
    return Color(0, 0, 0);
}

Color gray_text()
{
    return Color(128, 128, 128);
}

Color highlight()
{
    return Color(61, 174, 233);
}

Color highlight_text()
{
    return Color(255, 255, 255);
}

Color link_text()
{
    return Color(0, 0, 238);
}

Color mark()
{
    return Color(255, 255, 0);
}

Color mark_text()
{
    return Color(0, 0, 0);
}

Color selected_item()
{
    return Color(61, 174, 233);
}

Color selected_item_text()
{
    return Color(255, 255, 255);
}

Color visited_text()
{
    return Color(85, 26, 139);
}

}
