/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/PlatformColors.h>

namespace WebView {

Gfx::Color macos_web_selection_color()
{
    return Gfx::Color(128, 188, 254, 153);
}

Gfx::Color macos_web_inactive_selection_color()
{
    return Gfx::Color(183, 183, 183, 153);
}

Gfx::Color macos_web_inactive_selection_text_color()
{
    return Gfx::Color::Black;
}

}
