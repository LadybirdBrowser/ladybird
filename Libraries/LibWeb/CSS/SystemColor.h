/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWeb/CSS/PreferredColorScheme.h>
#include <LibWeb/Export.h>

// https://www.w3.org/TR/css-color-4/#css-system-colors
namespace Web::CSS::SystemColor {

Gfx::Color accent_color(PreferredColorScheme);
Gfx::Color accent_color_text(PreferredColorScheme);
Gfx::Color active_text(PreferredColorScheme);
Gfx::Color button_border(PreferredColorScheme);
Gfx::Color button_face(PreferredColorScheme);
Gfx::Color button_text(PreferredColorScheme);
Gfx::Color canvas(PreferredColorScheme);
Gfx::Color canvas_text(PreferredColorScheme);
Gfx::Color field(PreferredColorScheme);
Gfx::Color field_text(PreferredColorScheme);
Gfx::Color gray_text(PreferredColorScheme);
WEB_API Gfx::Color highlight(PreferredColorScheme);
WEB_API Gfx::Color highlight_text(PreferredColorScheme);
Gfx::Color link_text(PreferredColorScheme);
Gfx::Color mark(PreferredColorScheme);
Gfx::Color mark_text(PreferredColorScheme);
Gfx::Color selected_item(PreferredColorScheme);
Gfx::Color selected_item_text(PreferredColorScheme);
Gfx::Color visited_text(PreferredColorScheme);

}
