/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <LibWeb/Forward.h>

namespace WebContent::DevToolsIndexedDB {

JsonObject serialize_storage(Web::DOM::Document&);
JsonObject serialize_objects(Web::DOM::Document&, String const& host, JsonValue const& names, JsonValue const& options);
ErrorOr<JsonObject> delete_database(Web::DOM::Document&, String const& host, String const& name);
ErrorOr<JsonObject> clear_object_store(Web::DOM::Document&, String const& host, String const& name);
ErrorOr<JsonObject> delete_record(Web::DOM::Document&, String const& host, String const& name);

}
