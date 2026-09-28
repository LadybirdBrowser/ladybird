/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/WebDriver/Error.h>

namespace Web::WebDriver {

WEBCOMMON_API bool has_proxy_configuration();
WEBCOMMON_API void set_has_proxy_configuration(bool);
WEBCOMMON_API void reset_has_proxy_configuration();

WEBCOMMON_API ErrorOr<JsonObject, Error> deserialize_as_a_proxy(JsonValue const&);

}
