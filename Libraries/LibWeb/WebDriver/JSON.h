/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

WEB_API Response json_clone(HTML::BrowsingContext const&, JS::Value);
WEB_API ErrorOr<JS::Value, WebDriver::Error> json_deserialize(HTML::BrowsingContext const&, JsonValue const&);

}
