/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibWebCommon/WebDriver/Input.h>

namespace Web::WebDriver {

Response unwrap_input_command_response(JsonValue const& value, JsonValue& input_state)
{
    auto malformed_response = [] {
        return Error::from_code(ErrorCode::UnknownError, "Malformed input command response"sv);
    };
    if (!value.is_object())
        return malformed_response();
    auto const& envelope = value.as_object();
    auto state = envelope.get_object("__webdriver_input_state"sv);
    if (!state.has_value() || !state->get_array("sources"sv).has_value() || !state->get_array("cancel"sv).has_value())
        return malformed_response();

    auto result = envelope.get("value"sv);
    auto error_value = envelope.get("error"sv);
    if (result.has_value() == error_value.has_value())
        return malformed_response();

    Response response;
    if (error_value.has_value()) {
        if (!error_value->is_object())
            return malformed_response();
        auto const& error = error_value->as_object();
        auto http_status = error.get_u32("httpStatus"sv);
        auto error_code = error.get_string("error"sv);
        auto message = error.get_string("message"sv);
        if (!http_status.has_value() || *http_status < 400 || *http_status > 599 || !error_code.has_value() || error_code->is_empty() || !message.has_value())
            return malformed_response();
        Optional<JsonValue> data;
        if (auto data_value = error.get("data"sv); data_value.has_value())
            data = *data_value;
        response = Error { *http_status, *error_code, *message, move(data) };
    } else {
        response = JsonValue { *result };
    }

    input_state = JsonValue { *state };
    return response;
}

}
