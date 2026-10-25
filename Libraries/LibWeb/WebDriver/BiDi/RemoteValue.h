/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebDriver/BiDi.h>
#include <LibWebCommon/WebDriver/Error.h>

namespace Web::WebDriver::BiDi {

// https://w3c.github.io/webdriver-bidi/#type-script-ResultOwnership
enum class ResultOwnership : u8 {
    Root,
    None,
};

ErrorOr<ResultOwnership, Error> parse_result_ownership(JsonValue const&);

// https://w3c.github.io/webdriver-bidi/#realm-id
String realm_id(HTML::EnvironmentSettingsObject&);

// The realm whose id is realm id, if the document of the given navigable has it.
HTML::EnvironmentSettingsObject* settings_with_realm_id(HTML::Navigable&, StringView realm_id);

// https://w3c.github.io/webdriver-bidi/#handle-object-map
void clear_handle_object_map(HTML::EnvironmentSettingsObject&);
// NB: The handles of a session that ended name nothing.
void clear_all_handle_object_maps();
bool remove_handle(HTML::EnvironmentSettingsObject&, StringView handle_id);

// https://w3c.github.io/webdriver-bidi/#serialize-as-a-remote-value
// NB: The serialization internal map is internal to the serialization, which takes a value and returns its remote value.
JsonValue serialize_as_a_remote_value(JS::Value, SerializationOptions const&, ResultOwnership, HTML::EnvironmentSettingsObject&);

// https://w3c.github.io/webdriver-bidi/#deserialize-shared-reference
ErrorOr<JS::Value, Error> deserialize_shared_reference(JsonObject const& shared_reference, HTML::EnvironmentSettingsObject&);

// https://w3c.github.io/webdriver-bidi/#deserialize-local-value
ErrorOr<JS::Value, Error> deserialize_local_value(JsonValue const& local_protocol_value, HTML::EnvironmentSettingsObject&);

// https://w3c.github.io/webdriver-bidi/#stack-trace-for-an-exception
JsonObject stack_trace_for_an_exception(JS::Value exception);

// https://w3c.github.io/webdriver-bidi/#current-stack-trace
JsonObject current_stack_trace(JS::VM&);

// NB: The implementation-defined textual description of an exception that get the exception details gives.
String textual_description_of_an_exception(HTML::EnvironmentSettingsObject&, JS::Value exception);

// https://w3c.github.io/webdriver-bidi/#get-exception-details
JsonObject get_exception_details(HTML::EnvironmentSettingsObject&, JS::Value exception, ResultOwnership);

}
