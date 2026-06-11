/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibDevTools/IndexedDBSerialization.h>
#include <LibDevTools/StorageHelpers.h>
#include <LibURL/URL.h>
#include <LibWeb/IndexedDB/Inspection.h>

namespace DevTools::IndexedDB {

static constexpr auto indexed_database_default_storage_name = "default"sv;
static constexpr auto indexed_database_default_storage_suffix = " (default)"sv;

static String database_name_for_devtools(String const& database_name)
{
    return MUST(String::formatted("{}{}", database_name, indexed_database_default_storage_suffix));
}

static String database_name_from_devtools(String const& name)
{
    auto view = name.bytes_as_string_view();
    if (view.ends_with(indexed_database_default_storage_suffix))
        return MUST(String::from_utf8(view.substring_view(0, view.length() - indexed_database_default_storage_suffix.length())));
    return name;
}

static String indexed_database_path(String const& database_name, Optional<String const&> object_store_name = {}, Optional<JsonValue const&> key = {})
{
    JsonArray path;
    path.must_append(database_name_for_devtools(database_name));
    if (object_store_name.has_value())
        path.must_append(*object_store_name);
    if (key.has_value())
        path.must_append(*key);
    return path.serialized();
}

JsonObject serialize_storage(Web::DOM::Document& document)
{
    JsonObject hosts;
    for (auto const& storage_host : Web::IndexedDB::inspect_indexed_database_storage(document)) {
        auto host = storage_host_for_url(storage_host.url);
        if (!host.has_value())
            continue;

        JsonArray names;
        for (auto const& database : storage_host.databases) {
            if (database.object_store_names.is_empty()) {
                names.must_append(indexed_database_path(database.name));
                continue;
            }

            for (auto const& object_store_name : database.object_store_names)
                names.must_append(indexed_database_path(database.name, object_store_name));
        }

        hosts.set(*host, move(names));
    }
    return hosts;
}

static Optional<Web::IndexedDB::InspectionPath> parse_indexed_database_path(JsonValue const& name)
{
    if (!name.is_string())
        return {};

    auto parsed = JsonValue::from_string(name.as_string());
    if (parsed.is_error() || !parsed.value().is_array())
        return {};

    auto array = parsed.release_value().as_array();
    if (array.is_empty() || !array.at(0).is_string())
        return {};

    Web::IndexedDB::InspectionPath path;
    path.database_name = database_name_from_devtools(array.at(0).as_string());
    if (array.size() >= 2) {
        if (!array.at(1).is_string())
            return {};
        path.object_store_name = array.at(1).as_string();
    }
    if (array.size() >= 3)
        path.key = array.at(2);
    return path;
}

static Optional<Vector<Web::IndexedDB::InspectionPath>> parse_indexed_database_paths(Optional<JsonArray> const& names)
{
    if (!names.has_value())
        return {};

    Vector<Web::IndexedDB::InspectionPath> paths;
    for (auto const& name : names->values()) {
        auto path = parse_indexed_database_path(name);
        if (path.has_value())
            paths.append(path.release_value());
    }
    if (paths.is_empty())
        return {};
    return paths;
}

static Function<bool(URL::URL const&)> host_filter(String const& host)
{
    return [host](URL::URL const& url) {
        auto storage_host = storage_host_for_url(url);
        return storage_host.has_value() && *storage_host == host;
    };
}

static JsonObject serialize_object_row(Web::IndexedDB::InspectionObject const& inspection_object, String const& host)
{
    return inspection_object.visit(
        [&](Web::IndexedDB::InspectionDatabaseRow const& row) {
            JsonObject object;
            object.set("uniqueKey"sv, database_name_for_devtools(row.name));
            object.set("db"sv, row.name);
            object.set("storage"sv, indexed_database_default_storage_name);
            object.set("origin"sv, host);
            object.set("version"sv, row.version);
            object.set("objectStores"sv, row.object_store_count);
            return object;
        },
        [](Web::IndexedDB::InspectionObjectStore const& store) {
            JsonArray indexes;
            for (auto const& index : store.indexes) {
                JsonObject object;
                object.set("name"sv, index.name);
                object.set("keyPath"sv, index.key_path);
                object.set("unique"sv, index.unique);
                object.set("multiEntry"sv, index.multi_entry);
                indexes.must_append(move(object));
            }
            JsonObject object;
            object.set("objectStore"sv, store.name);
            if (store.key_path.has_value())
                object.set("keyPath"sv, *store.key_path);
            else
                object.set("keyPath"sv, JsonValue {});
            object.set("autoIncrement"sv, store.auto_increment);
            object.set("indexes"sv, indexes.serialized());
            return object;
        },
        [](Web::IndexedDB::InspectionRecord const& record) {
            JsonObject object;
            object.set("name"sv, record.key);
            object.set("value"sv, record.value);
            return object;
        });
}

static JsonObject paginated_indexed_database_response(Vector<Web::IndexedDB::InspectionObject> rows, JsonObject const& options, String const& host)
{
    static constexpr auto max_objects_per_page = 50uz;

    size_t offset = options.get_integer<size_t>("offset"sv).value_or(0);
    auto size = min(options.get_integer<size_t>("size"sv).value_or(max_objects_per_page), max_objects_per_page);
    auto total = rows.size();

    JsonArray data;
    if (offset < total) {
        auto end = min(total, offset + size);
        for (size_t i = offset; i < end; ++i)
            data.must_append(serialize_object_row(rows.at(i), host));
    } else {
        offset = total;
    }

    JsonObject response;
    response.set("offset"sv, offset);
    response.set("total"sv, total);
    response.set("data"sv, move(data));
    return response;
}

JsonObject serialize_objects(Web::DOM::Document& document, String const& host, JsonValue const& names, JsonValue const& options)
{
    auto parsed_options = options.is_object() ? options.as_object() : JsonObject {};
    auto parsed_named = names.is_array() ? names.as_array() : JsonArray {};
    return paginated_indexed_database_response(
        Web::IndexedDB::inspect_indexed_database_objects(document, host_filter(host), parse_indexed_database_paths(parsed_named)),
        parsed_options,
        host);
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
        paths.must_append(indexed_database_path(change.database_name, change.object_store_name, change.key));
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

static ErrorOr<Web::IndexedDB::InspectionPath> parse_required_indexed_database_path(String const& name)
{
    auto path = parse_indexed_database_path(JsonValue { name });
    if (!path.has_value())
        return Error::from_string_literal("Invalid IndexedDB path");
    return path.release_value();
}

ErrorOr<JsonObject> delete_database(Web::DOM::Document& document, String const& host, String const& name)
{
    auto blocked = TRY(Web::IndexedDB::delete_indexed_database_for_inspection(document, host_filter(host), database_name_from_devtools(name)));

    JsonObject response;
    if (blocked)
        response.set("blocked"sv, true);
    return response;
}

ErrorOr<JsonObject> clear_object_store(Web::DOM::Document& document, String const& host, String const& name)
{
    auto path = TRY(parse_required_indexed_database_path(name));
    if (!path.object_store_name.has_value() || path.key.has_value())
        return Error::from_string_literal("Invalid IndexedDB object store path");

    TRY(Web::IndexedDB::clear_indexed_database_object_store_for_inspection(document, host_filter(host), path.database_name, *path.object_store_name));
    return JsonObject {};
}

ErrorOr<JsonObject> delete_record(Web::DOM::Document& document, String const& host, String const& name)
{
    auto path = TRY(parse_required_indexed_database_path(name));
    if (!path.object_store_name.has_value() || !path.key.has_value())
        return Error::from_string_literal("Invalid IndexedDB record path");

    TRY(Web::IndexedDB::delete_indexed_database_record_for_inspection(document, host_filter(host), path.database_name, *path.object_store_name, *path.key));
    return JsonObject {};
}

}
