/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <AK/String.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <WebContent/Forward.h>

namespace WebContent {

// https://w3c.github.io/webdriver-bidi/#commands
// NB: Runs a command of a BiDi session against a navigable this page hosts, which the browser process names.
void run_webdriver_bidi_command(PageClient&, u64 command_id, Web::HTML::CrossProcessId navigable_id, String const& method, JsonValue params);

void set_webdriver_bidi_session_state(Web::WebDriver::BiDiSessionState);

}
