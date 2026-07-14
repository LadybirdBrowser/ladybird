/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace Ladybird {

static constexpr int NATIVE_MACOS_WINDOW_CONTROLS_WIDTH = 62;
static constexpr int NATIVE_MACOS_WINDOW_CONTROLS_GAP = 16;

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
        .expanded_sidebar_width = 232,
        .collapsed_sidebar_width = 52,
        .toolbar_height = 40,
    };
}

static constexpr bool show_menubar_option_available()
{
    return !use_native_macos_window_controls();
}

}
