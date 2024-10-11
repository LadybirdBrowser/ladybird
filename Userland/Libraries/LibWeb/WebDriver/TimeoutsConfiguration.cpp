/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-timeouts-object
JsonObject timeouts_object(TimeoutsConfiguration const& timeouts)
{
    // 1. Let serialized be an empty map.
    JsonObject serialized;

    // 2. Set serialized["script"] to timeouts' script timeout.
    serialized.set("script"sv, timeouts.script_timeout.has_value() ? *timeouts.script_timeout : JsonValue {});

    // 3. Set serialized["pageLoad"] to timeouts' page load timeout.
    serialized.set("pageLoad"sv, timeouts.page_load_timeout.has_value() ? *timeouts.page_load_timeout : JsonValue {});

    // 4. Set serialized["implicit"] to timeouts' implicit wait timeout.
    serialized.set("implicit"sv, timeouts.implicit_wait_timeout.has_value() ? *timeouts.implicit_wait_timeout : JsonValue {});

    // 5. Return convert an Infra value to a JSON-compatible JavaScript value with serialized.
    return serialized;
}

// https://w3c.github.io/webdriver/#dfn-deserialize-as-timeouts-configuration
ErrorOr<TimeoutsConfiguration, Error> json_deserialize_as_a_timeouts_configuration(JsonValue const& timeouts)
{
    // 2. Let configuration be a new timeouts configuration.
    TimeoutsConfiguration configuration {};

    TRY(json_deserialize_as_a_timeouts_configuration_into(timeouts, configuration));

    // 4. Return success with data configuration.
    return configuration;
}

// https://w3c.github.io/webdriver/#dfn-deserialize-as-timeouts-configuration
ErrorOr<void, Error> json_deserialize_as_a_timeouts_configuration_into(JsonValue const& timeouts, TimeoutsConfiguration& configuration)
{
    // 1. Set timeouts to the result of converting a JSON-derived JavaScript value to an Infra value with timeouts.
    if (!timeouts.is_object())
        return Error::from_code(ErrorCode::InvalidArgument, "Payload is not a JSON object");

    // 3. For each key → value in timeouts:
    TRY(timeouts.as_object().try_for_each_member([&](auto const& key, JsonValue const& value) -> ErrorOr<void, Error> {
        Optional<u64> parsed_value;

        // 1. If «"script", "pageLoad", "implicit"» does not contain key, then continue.
        if (!key.is_one_of("script"sv, "pageLoad"sv, "implicit"sv))
            return {};

        // 2. If value is neither null nor a number greater than or equal to 0 and less than or equal to the maximum
        //    safe integer return error with error code invalid argument.
        if (!value.is_null()) {
            auto duration = value.get_integer<u64>();

            if (!duration.has_value() || *duration > JS::MAX_ARRAY_LIKE_INDEX)
                return Error::from_code(ErrorCode::InvalidArgument, "Invalid timeout value");

            parsed_value = static_cast<u64>(*duration);
        }

        // 3. Run the substeps matching key:
        // -> "script"
        if (key == "script"sv) {
            // Set configuration's script timeout to value.
            configuration.script_timeout = parsed_value;
        }
        // -> "pageLoad"
        else if (key == "pageLoad"sv) {
            // Set configuration's page load timeout to value.
            configuration.page_load_timeout = parsed_value;
        }
        // -> "implicit"
        else if (key == "implicit"sv) {
            // Set configuration's implicit wait timeout to value.
            configuration.implicit_wait_timeout = parsed_value;
        }

        return {};
    }));

    return {};
}

}
