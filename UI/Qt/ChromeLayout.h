/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace Ladybird {

enum class WindowControlsPlacement {
    LeftTrafficLights,
    RightCustomControls,
};

struct BrowserChromeLayoutPolicy {
    WindowControlsPlacement controls_placement;
    int expanded_sidebar_width;
    int collapsed_sidebar_width;
    int toolbar_height;
};

static constexpr BrowserChromeLayoutPolicy browser_chrome_layout_policy()
{
    return {
#if defined(AK_OS_MACOS)
        .controls_placement = WindowControlsPlacement::LeftTrafficLights,
#else
        .controls_placement = WindowControlsPlacement::RightCustomControls,
#endif
        .expanded_sidebar_width = 232,
        .collapsed_sidebar_width = 52,
        .toolbar_height = 42,
    };
}

static constexpr bool use_left_traffic_light_window_controls()
{
    return browser_chrome_layout_policy().controls_placement == WindowControlsPlacement::LeftTrafficLights;
}

static constexpr bool use_right_custom_window_controls()
{
    return browser_chrome_layout_policy().controls_placement == WindowControlsPlacement::RightCustomControls;
}

static constexpr bool show_menubar_option_available()
{
    return use_right_custom_window_controls();
}

}
