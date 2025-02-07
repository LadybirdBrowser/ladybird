/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

bool has_proxy_configuration();
void set_has_proxy_configuration(bool);
void reset_has_proxy_configuration();

ErrorOr<JsonObject, Error> deserialize_as_a_proxy(JsonValue const&);

}
