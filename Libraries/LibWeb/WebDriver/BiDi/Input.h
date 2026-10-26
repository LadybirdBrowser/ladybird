/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/BiDi/Script.h>

// https://w3c.github.io/webdriver-bidi/#module-input
namespace Web::WebDriver::BiDi {

// https://w3c.github.io/webdriver-bidi/#command-input-performActions
WEB_API void perform_actions(HTML::Navigable&, JsonObject const& params, OnCommandComplete);

// https://w3c.github.io/webdriver-bidi/#command-input-releaseActions
WEB_API void release_actions(HTML::Navigable&, OnCommandComplete);

}
