/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <LibWebCommon/WebDriver/Response.h>

namespace Web::WebDriver {

// Commands which share the session's input state, regardless of protocol.
inline bool is_input_state_command(StringView name)
{
    return name == "input.performActions"sv || name == "input.releaseActions"sv
        || name == "perform_actions"sv || name == "release_actions"sv
        || name == "element_click"sv || name == "element_send_keys"sv;
}

// Decode the renderer's private input-command envelope. Commit the snapshot only after the whole reply is valid.
WEBCOMMON_API Response unwrap_input_command_response(JsonValue const&, JsonValue& input_state);

}
