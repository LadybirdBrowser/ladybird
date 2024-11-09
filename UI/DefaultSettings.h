/*
 * Copyright (c) 2023, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Browser {

static constexpr StringView default_homepage_url = "resource://html/misc/welcome.html"sv;
static constexpr StringView default_new_tab_url = "about:newtab"sv;
static constexpr StringView default_color_scheme = "auto"sv;
static constexpr bool default_enable_content_filters = true;
static constexpr bool default_show_bookmarks_bar = true;
static constexpr bool default_close_download_widget_on_finish = false;
static constexpr bool default_allow_autoplay_on_all_websites = false;

}
