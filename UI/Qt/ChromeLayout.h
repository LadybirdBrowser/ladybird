/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace Ladybird {

// The traffic lights sit this far in from the left edge and the top edge of the window, which puts them on the line
// that the window's own items on the tab strip are centered on.
static constexpr int NATIVE_MACOS_WINDOW_CONTROLS_INSET = 12;
// The three traffic lights, with the room after the last one.
static constexpr int NATIVE_MACOS_WINDOW_CONTROLS_WIDTH = 56;
static constexpr int NATIVE_MACOS_WINDOW_CONTROLS_GAP = 14;
// The Ladybird button keeps 24 px clear to the traffic lights before it.
// NB: The traffic lights reach 3 px past the room kept for them.
static constexpr int LADYBIRD_BUTTON_TRAFFIC_LIGHTS_GAP = 27;

static constexpr bool use_native_macos_window_controls()
{
#if defined(AK_OS_MACOS)
    return true;
#else
    return false;
#endif
}

struct BrowserChromeLayoutPolicy {
    int expanded_sidebar_width;
    int collapsed_sidebar_width;
    int toolbar_height;
};

static constexpr BrowserChromeLayoutPolicy browser_chrome_layout_policy()
{
    return {
        .expanded_sidebar_width = 236,
        .collapsed_sidebar_width = 52,
        .toolbar_height = 40,
    };
}

static constexpr bool show_menu_bar_option_available()
{
    return !use_native_macos_window_controls();
}

}
