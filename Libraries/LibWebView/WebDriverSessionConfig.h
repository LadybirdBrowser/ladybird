/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <LibWeb/WebDriver/Capabilities.h>
#include <LibWeb/WebDriver/UserPrompt.h>

namespace WebView {

struct WebDriverSessionConfig {
    Web::WebDriver::UserPromptHandler user_prompt_handler;
    Web::WebDriver::PageLoadStrategy page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };
    bool strict_file_interactability { false };
    JsonValue timeouts;
};

}
