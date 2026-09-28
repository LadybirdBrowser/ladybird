/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <LibDevTools/Forward.h>
#include <LibWebCommon/IndexedDB/TransactionChanges.h>

namespace DevTools::IndexedDB {

DEVTOOLS_API String database_name_for_devtools(String const& database_name);
DEVTOOLS_API String database_name_from_devtools(String const& name);
DEVTOOLS_API String indexed_database_path(String const& database_name, Optional<String const&> object_store_name = {}, Optional<JsonValue const&> key = {});

DEVTOOLS_API JsonObject serialize_update(String const& url, Web::IndexedDB::TransactionChanges const&);

}
