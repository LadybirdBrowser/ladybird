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

// https://w3c.github.io/webdriver-bidi/#module-browsingContext
// NB: The commands of the module that run in the process hosting a navigable's active document.
namespace Web::WebDriver::BiDi {

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-captureScreenshot
WEB_API void capture_screenshot(HTML::Navigable&, JsonObject const& params, OnCommandComplete);

// https://w3c.github.io/webdriver-bidi/#command-browsingContext-locateNodes
WEB_API Response locate_nodes(HTML::Navigable&, JsonObject const& params);

}
