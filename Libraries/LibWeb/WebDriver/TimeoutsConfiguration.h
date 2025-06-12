/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Optional.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-timeouts-configuration
struct TimeoutsConfiguration {
    Optional<u64> script_timeout { 30'000 };
    Optional<u64> page_load_timeout { 300'000 };
    Optional<u64> implicit_wait_timeout { 0 };
};

JsonObject timeouts_object(TimeoutsConfiguration const&);
ErrorOr<TimeoutsConfiguration, Error> json_deserialize_as_a_timeouts_configuration(JsonValue const&);
ErrorOr<void, Error> json_deserialize_as_a_timeouts_configuration_into(JsonValue const&, TimeoutsConfiguration&);

}
