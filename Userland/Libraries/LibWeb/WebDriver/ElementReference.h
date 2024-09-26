/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Error.h>

namespace Web::WebDriver {

ByteString get_or_create_a_web_element_reference(Web::DOM::Node const& element);
JsonObject web_element_reference_object(Web::DOM::Node const& element);
ByteString extract_web_element_reference(JsonObject const&);
bool represents_a_web_element(JsonValue const& value);
ErrorOr<Web::DOM::Element*, Web::WebDriver::Error> get_known_connected_element(StringView element_id);
bool is_element_stale(Web::DOM::Node const& element);

ByteString get_or_create_a_shadow_root_reference(Web::DOM::ShadowRoot const& shadow_root);
JsonObject shadow_root_reference_object(Web::DOM::ShadowRoot const& shadow_root);
ErrorOr<Web::DOM::ShadowRoot*, Web::WebDriver::Error> get_known_shadow_root(StringView shadow_id);

}
