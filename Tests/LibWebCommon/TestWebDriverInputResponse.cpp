/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/WebDriver/Input.h>

static JsonValue parse(StringView json)
{
    return MUST(JsonValue::from_string(json));
}

TEST_CASE(malformed_input_replies_preserve_the_previous_snapshot)
{
    auto state = parse(R"({"sources":[],"cancel":[],"previous":true})"sv);
    auto original_state = state.serialized();
    StringView const malformed_replies[] = {
        "null"sv,
        "[]"sv,
        "{}"sv,
        R"({"value":null})"sv,
        R"({"__webdriver_input_state":null,"value":null})"sv,
        R"({"__webdriver_input_state":{},"value":null})"sv,
        R"({"__webdriver_input_state":{"sources":null,"cancel":[]},"value":null})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":null},"value":null})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":null})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":[]})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"error":"invalid argument","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"error":"invalid argument"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":"400","error":"invalid argument","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400.5,"error":"invalid argument","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":200,"error":"invalid argument","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":600,"error":"invalid argument","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"error":false,"message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"error":"","message":"bad"}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"error":"invalid argument","message":null}})"sv,
        R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"value":null,"error":{"httpStatus":400,"error":"invalid argument","message":"bad"}})"sv,
    };
    for (auto const& json : malformed_replies) {
        auto response = Web::WebDriver::unwrap_input_command_response(parse(json), state);
        EXPECT(response.is_error());
        if (response.is_error()) {
            EXPECT_EQ(response.error().http_status, 500u);
            EXPECT_EQ(response.error().error, "unknown error"sv);
        }
        EXPECT_EQ(state.serialized(), original_state);
    }

    // A valid reply following malformed replies still completes and commits its own snapshot.
    auto response = Web::WebDriver::unwrap_input_command_response(parse(R"({"__webdriver_input_state":{"sources":[],"cancel":[],"next":true},"value":null})"sv), state);
    EXPECT(!response.is_error());
    EXPECT(response.value().is_null());
    EXPECT_EQ(state.as_object().get_bool("next"sv), true);
    EXPECT(!state.as_object().has("previous"sv));
}

TEST_CASE(valid_error_reply_commits_state_and_preserves_error_data)
{
    JsonValue state;
    auto response = Web::WebDriver::unwrap_input_command_response(parse(R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"error":{"httpStatus":400,"error":"invalid argument","message":"","data":{"partial":true}}})"sv), state);
    EXPECT(response.is_error());
    EXPECT_EQ(response.error().http_status, 400u);
    EXPECT_EQ(response.error().error, "invalid argument"sv);
    EXPECT_EQ(response.error().message, ""sv);
    EXPECT(response.error().data.has_value());
    EXPECT_EQ(response.error().data->as_object().get_bool("partial"sv), true);
    EXPECT(state.is_object());
    EXPECT(state.as_object().get_array("sources"sv).has_value());
}

TEST_CASE(success_reply_preserves_value_without_interpreting_its_fields)
{
    JsonValue state;
    auto response = Web::WebDriver::unwrap_input_command_response(parse(R"({"__webdriver_input_state":{"sources":[],"cancel":[]},"value":{"error":42}})"sv), state);
    EXPECT(!response.is_error());
    EXPECT_EQ(response.value().as_object().get_u32("error"sv), 42u);
    EXPECT(state.is_object());
}
