/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/IndexedDB/TransactionChanges.h>

namespace DevTools::IndexedDB {

DEVTOOLS_API JsonObject serialize_storage(Web::DOM::Document&);
DEVTOOLS_API JsonObject serialize_objects(Web::DOM::Document&, String const& host, JsonValue const& names, JsonValue const& options);
DEVTOOLS_API JsonObject serialize_update(String const& url, Web::IndexedDB::TransactionChanges const&);
DEVTOOLS_API ErrorOr<JsonObject> delete_database(Web::DOM::Document&, String const& host, String const& name);
DEVTOOLS_API ErrorOr<JsonObject> clear_object_store(Web::DOM::Document&, String const& host, String const& name);
DEVTOOLS_API ErrorOr<JsonObject> delete_record(Web::DOM::Document&, String const& host, String const& name);

}
