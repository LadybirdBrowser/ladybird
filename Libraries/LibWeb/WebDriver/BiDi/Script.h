/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebDriver/Response.h>

// https://w3c.github.io/webdriver-bidi/#module-script
namespace Web::WebDriver::BiDi {

using OnCommandComplete = Function<void(Response)>;

// https://w3c.github.io/webdriver-bidi/#command-script-evaluate
WEB_API void evaluate(HTML::Navigable&, JsonObject const& params, OnCommandComplete);

// https://w3c.github.io/webdriver-bidi/#command-script-callFunction
WEB_API void call_function(HTML::Navigable&, JsonObject const& params, OnCommandComplete);

// https://w3c.github.io/webdriver-bidi/#command-script-disown
WEB_API Response disown(HTML::Navigable&, JsonObject const& params);

// https://w3c.github.io/webdriver-bidi/#command-script-getRealms
// NB: The realms of the given navigable's active document, which the browser process gathers for each navigable.
WEB_API Response get_realms(HTML::Navigable&, JsonObject const& params);

// https://w3c.github.io/webdriver-bidi/#get-the-realm-info
Optional<JsonObject> get_the_realm_info(HTML::EnvironmentSettingsObject&);

// https://w3c.github.io/webdriver-bidi/#get-the-source
JsonObject get_the_source(HTML::EnvironmentSettingsObject&);

// https://w3c.github.io/webdriver-bidi/#create-a-channel
JS::Value create_a_channel(HTML::EnvironmentSettingsObject&, JsonObject const& protocol_value);

// NB: The evaluations that await a promise of the realm, which is destroyed, complete with an error.
void fail_pending_evaluations(HTML::EnvironmentSettingsObject&);
void fail_all_pending_evaluations();

// https://w3c.github.io/webdriver-bidi/#run-webdriver-bidi-preload-scripts
void run_webdriver_bidi_preload_scripts(HTML::EnvironmentSettingsObject&);

}
