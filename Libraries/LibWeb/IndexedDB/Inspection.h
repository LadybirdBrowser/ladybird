/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::IndexedDB {

struct InspectionDatabase {
    String name;
    u64 version;
    Vector<String> object_store_names;
};

struct InspectionStorageHost {
    String url;
    Vector<InspectionDatabase> databases;
};

struct InspectionDatabaseRow {
    String name;
    u64 version;
    size_t object_store_count { 0 };
};

struct InspectionIndex {
    String name;
    String key_path;
    bool unique { false };
    bool multi_entry { false };
};

struct InspectionObjectStore {
    String name;
    Optional<String> key_path;
    bool auto_increment { false };
    Vector<InspectionIndex> indexes;
};

struct InspectionRecord {
    JsonValue key;
    String value;
};

using InspectionObject = Variant<InspectionDatabaseRow, InspectionObjectStore, InspectionRecord>;

struct InspectionPath {
    String database_name;
    Optional<String> object_store_name;
    Optional<JsonValue> key;
};

WEB_API JsonValue serialize_key_for_inspection(Key&);
WEB_API Vector<InspectionStorageHost> inspect_indexed_database_storage(DOM::Document&);
WEB_API Vector<InspectionObject> inspect_indexed_database_objects(DOM::Document&, Function<bool(URL::URL const&)> const& document_matches, Optional<Vector<InspectionPath>> const& paths);
WEB_API ErrorOr<bool> delete_indexed_database_for_inspection(DOM::Document&, Function<bool(URL::URL const&)> const& document_matches, String const& database_name);
WEB_API ErrorOr<void> clear_indexed_database_object_store_for_inspection(DOM::Document&, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name);
WEB_API ErrorOr<void> delete_indexed_database_record_for_inspection(DOM::Document&, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name, JsonValue const& key);

}
