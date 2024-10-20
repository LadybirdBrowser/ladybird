/*
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-web-window-identifier
inline constexpr auto WEB_WINDOW_IDENTIFIER = "window-fcc6-11e5-b4f8-330a88ab9d7f"sv;

// https://w3c.github.io/webdriver/#dfn-web-frame-identifier
inline constexpr auto WEB_FRAME_IDENTIFIER = "frame-075b-4da1-b6ba-e579c2d3230a"sv;

JsonObject window_proxy_reference_object(HTML::WindowProxy const&);

}
