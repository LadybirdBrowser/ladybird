/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/StringView.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

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

// https://w3c.github.io/webdriver/#dfn-unhandled-prompt-behavior
enum class UnhandledPromptBehavior {
    Dismiss,
    Accept,
    DismissAndNotify,
    AcceptAndNotify,
    Ignore,
};

constexpr UnhandledPromptBehavior unhandled_prompt_behavior_from_string(StringView behavior)
{
    if (behavior == "dismiss"sv)
        return UnhandledPromptBehavior::Dismiss;
    if (behavior == "accept"sv)
        return UnhandledPromptBehavior::Accept;
    if (behavior == "dismiss and notify"sv)
        return UnhandledPromptBehavior::DismissAndNotify;
    if (behavior == "accept and notify"sv)
        return UnhandledPromptBehavior::AcceptAndNotify;
    if (behavior == "ignore"sv)
        return UnhandledPromptBehavior::Ignore;
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

Response process_capabilities(JsonValue const& parameters);

}
