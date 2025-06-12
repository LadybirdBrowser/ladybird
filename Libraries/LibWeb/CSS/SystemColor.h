/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/CSS/PreferredColorScheme.h>

// https://www.w3.org/TR/css-color-4/#css-system-colors
namespace Web::CSS::SystemColor {

Color accent_color(PreferredColorScheme);
Color accent_color_text(PreferredColorScheme);
Color active_text(PreferredColorScheme);
Color button_border(PreferredColorScheme);
Color button_face(PreferredColorScheme);
Color button_text(PreferredColorScheme);
Color canvas(PreferredColorScheme);
Color canvas_text(PreferredColorScheme);
Color field(PreferredColorScheme);
Color field_text(PreferredColorScheme);
Color gray_text(PreferredColorScheme);
Color highlight(PreferredColorScheme);
Color highlight_text(PreferredColorScheme);
Color link_text(PreferredColorScheme);
Color mark(PreferredColorScheme);
Color mark_text(PreferredColorScheme);
Color selected_item(PreferredColorScheme);
Color selected_item_text(PreferredColorScheme);
Color visited_text(PreferredColorScheme);

}
