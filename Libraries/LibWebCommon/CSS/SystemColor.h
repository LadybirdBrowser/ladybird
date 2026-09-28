/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibWebCommon/CSS/PreferredColorScheme.h>
#include <LibWebCommon/Export.h>

// https://www.w3.org/TR/css-color-4/#css-system-colors
namespace Web::CSS::SystemColor {

WEBCOMMON_API Color accent_color(PreferredColorScheme);
WEBCOMMON_API Color accent_color_text(PreferredColorScheme);
WEBCOMMON_API Color active_text(PreferredColorScheme);
WEBCOMMON_API Color button_border(PreferredColorScheme);
WEBCOMMON_API Color button_face(PreferredColorScheme);
WEBCOMMON_API Color button_text(PreferredColorScheme);
WEBCOMMON_API Color canvas(PreferredColorScheme);
WEBCOMMON_API Color canvas_text(PreferredColorScheme);
WEBCOMMON_API Color field(PreferredColorScheme);
WEBCOMMON_API Color field_text(PreferredColorScheme);
WEBCOMMON_API Color gray_text(PreferredColorScheme);
WEBCOMMON_API Color transform_selection_background_color(Color);
WEBCOMMON_API Color highlight(PreferredColorScheme);
// A muted highlight for selections in windows that do not have focus.
WEBCOMMON_API Color inactive_highlight(PreferredColorScheme);
WEBCOMMON_API Color highlight_text(PreferredColorScheme);
WEBCOMMON_API Color link_text(PreferredColorScheme);
WEBCOMMON_API Color mark(PreferredColorScheme);
WEBCOMMON_API Color mark_text(PreferredColorScheme);
WEBCOMMON_API Color selected_item(PreferredColorScheme);
WEBCOMMON_API Color selected_item_text(PreferredColorScheme);
WEBCOMMON_API Color visited_text(PreferredColorScheme);

}
