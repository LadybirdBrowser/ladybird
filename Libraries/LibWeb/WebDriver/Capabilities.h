/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Forward.h>
#include <AK/StringView.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

enum class SessionFlags {
    Default = 0x0,
    Http = 0x1,
};
AK_ENUM_BITWISE_OPERATORS(SessionFlags);

// https://w3c.github.io/webdriver/#dfn-page-load-strategy
enum class PageLoadStrategy {
    None,
    Eager,
    Normal,
};

constexpr PageLoadStrategy page_load_strategy_from_string(StringView strategy)
{
    if (strategy == "none"sv)
        return PageLoadStrategy::None;
    if (strategy == "eager"sv)
        return PageLoadStrategy::Eager;
    if (strategy == "normal"sv)
        return PageLoadStrategy::Normal;
    VERIFY_NOT_REACHED();
}

enum class InterfaceMode {
    Graphical,
    Headless,
};
void set_default_interface_mode(InterfaceMode);

struct LadybirdOptions {
    explicit LadybirdOptions(JsonObject const& capabilities);

    bool headless { false };
};

Response process_capabilities(JsonValue const& parameters, SessionFlags flags);

}
