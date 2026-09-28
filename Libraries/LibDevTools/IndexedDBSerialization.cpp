/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/IndexedDBSerialization.h>
#include <LibDevTools/StorageHelpers.h>

namespace DevTools::IndexedDB {

static constexpr auto indexed_database_default_storage_suffix = " (default)"sv;

String database_name_for_devtools(String const& database_name)
{
    return MUST(String::formatted("{}{}", database_name, indexed_database_default_storage_suffix));
}

String database_name_from_devtools(String const& name)
{
    auto view = name.bytes_as_string_view();
    if (view.ends_with(indexed_database_default_storage_suffix))
        return MUST(String::from_utf8(view.substring_view(0, view.length() - indexed_database_default_storage_suffix.length())));
    return name;
}

String indexed_database_path(String const& database_name, Optional<String const&> object_store_name, Optional<JsonValue const&> key)
{
    JsonArray path;
    path.must_append(database_name_for_devtools(database_name));
    if (object_store_name.has_value())
        path.must_append(*object_store_name);
    if (key.has_value())
        path.must_append(*key);
    return path.serialized();
}

static void append_indexed_database_update(JsonObject& update, StringView type, JsonArray paths, String const& host)
{
    if (paths.is_empty())
        return;

    JsonObject hosts;
    hosts.set(host, move(paths));

    JsonObject indexed_database;
    indexed_database.set("indexedDB"sv, move(hosts));

    update.set(type, move(indexed_database));
}

static JsonArray serialize_update_paths(Vector<Web::IndexedDB::TransactionChange> const& changes)
{
    JsonArray paths;
    paths.ensure_capacity(changes.size());
    for (auto const& change : changes) {
        // FIXME: Firefox treats added IndexedDB update paths as storage tree additions, so record-level changes must
        //        not be sent there or they appear as blank rows in the selected host view.
        //        Remove this filter once Firefox supports record updates.
        if (change.key.has_value())
            continue;
        paths.must_append(indexed_database_path(change.database_name.to_utf8(), change.object_store_name.to_utf8(), change.key));
    }
    return paths;
}

JsonObject serialize_update(String const& url, Web::IndexedDB::TransactionChanges const& changes)
{
    JsonObject update;
    auto host = storage_host_for_url(url);
    if (!host.has_value())
        return update;

    append_indexed_database_update(update, "added"sv, serialize_update_paths(changes.added), *host);
    append_indexed_database_update(update, "changed"sv, serialize_update_paths(changes.changed), *host);
    append_indexed_database_update(update, "deleted"sv, serialize_update_paths(changes.deleted), *host);
    return update;
}

}
