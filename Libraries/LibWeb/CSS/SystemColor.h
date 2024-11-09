/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>

// https://www.w3.org/TR/css-color-4/#css-system-colors
namespace Web::CSS::SystemColor {

// FIXME: Provide colors for `color-scheme: dark` once we support that.
Color accent_color();
Color accent_color_text();
Color active_text();
Color button_border();
Color button_face();
Color button_text();
Color canvas();
Color canvas_text();
Color field();
Color field_text();
Color gray_text();
Color highlight();
Color highlight_text();
Color link_text();
Color mark();
Color mark_text();
Color selected_item();
Color selected_item_text();
Color visited_text();

}
