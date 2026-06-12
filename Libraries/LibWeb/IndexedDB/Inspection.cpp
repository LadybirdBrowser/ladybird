/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/Inspection.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/Database.h>
#include <LibWeb/IndexedDB/Internal/Index.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/IndexedDB/Internal/ObjectStore.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

struct IndexedDatabaseDocument {
    String url;
    GC::Ref<DOM::Document> document;
    StorageAPI::StorageKey storage_key;
};

JsonValue serialize_key_for_inspection(Key& key)
{
    switch (key.type()) {
    case Key::Invalid:
        return {};
    case Key::Number:
    case Key::Date:
        return key.value_as_double();
    case Key::String:
        return key.value_as_string();
    case Key::Binary:
        return key.dump();
    case Key::Array: {
        JsonArray keys;
        for (auto const& subkey : key.subkeys())
            keys.must_append(serialize_key_for_inspection(*subkey));
        return keys;
    }
    }

    VERIFY_NOT_REACHED();
}

static String serialize_key_path(KeyPath const& key_path)
{
    return key_path.visit(
        [](String const& value) {
            return value;
        },
        [](Vector<String> const& values) {
            JsonArray array;
            for (auto const& value : values)
                array.must_append(value);
            return array.serialized();
        });
}

static String serialize_record_value(JS::Realm& realm, HTML::SerializationRecord const& record)
{
    HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    auto value = HTML::structured_deserialize(realm.vm(), record, realm);
    if (value.is_exception())
        return "<unserializable>"_string;

    auto serialized = Infra::serialize_javascript_value_to_json_string(realm.vm(), value.release_value());
    if (serialized.is_exception())
        return "<unserializable>"_string;

    return serialized.release_value();
}

static Vector<IndexedDatabaseDocument> indexed_database_documents_for_inspection(DOM::Document& root_document)
{
    Vector<IndexedDatabaseDocument> documents;

    auto append_document = [&](DOM::Document& document) {
        auto storage_key = StorageAPI::obtain_a_storage_key(document.relevant_settings_object());
        if (!storage_key.has_value())
            return;

        documents.append({ document.url().serialize(), document, storage_key.release_value() });
    };

    append_document(root_document);
    root_document.for_each_in_subtree_of_type<HTML::NavigableContainer>([&](HTML::NavigableContainer& navigable_container) {
        auto content_navigable = navigable_container.content_navigable();
        if (!content_navigable)
            return TraversalDecision::Continue;

        auto content_document = content_navigable->active_document();
        if (!content_document)
            return TraversalDecision::Continue;

        if (!content_document->origin().is_same_origin_domain(navigable_container.document().origin()))
            return TraversalDecision::Continue;

        append_document(*content_document);
        return TraversalDecision::Continue;
    });

    return documents;
}

static Optional<IndexedDatabaseDocument> matching_document(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches)
{
    for (auto const& entry : indexed_database_documents_for_inspection(document)) {
        if (document_matches(entry.document->url()))
            return entry;
    }
    return {};
}

Vector<InspectionStorageHost> inspect_indexed_database_storage(DOM::Document& document)
{
    Vector<InspectionStorageHost> hosts;
    for (auto const& entry : indexed_database_documents_for_inspection(document)) {
        Vector<InspectionDatabase> databases;
        for (auto& database : Database::for_key(entry.storage_key)) {
            if (!database || database->version() == 0)
                continue;

            Vector<String> object_store_names;
            for (auto const& object_store : database->object_stores())
                object_store_names.append(object_store->name());

            databases.append({ database->name(), database->version(), move(object_store_names) });
        }

        hosts.append({ entry.url, move(databases) });
    }
    return hosts;
}

static InspectionObjectStore inspect_object_store(ObjectStore& object_store)
{
    Vector<InspectionIndex> indexes;
    for (auto const& entry : object_store.index_set()) {
        auto const& index = *entry.value;
        indexes.append({ index.name(), serialize_key_path(index.key_path()), index.unique(), index.multi_entry() });
    }

    Optional<String> key_path;
    if (auto value = object_store.key_path(); value.has_value())
        key_path = serialize_key_path(*value);

    return { object_store.name(), move(key_path), object_store.uses_a_key_generator(), move(indexes) };
}

static void append_database_rows(Vector<InspectionObject>& rows, StorageAPI::StorageKey const& storage_key)
{
    for (auto& database : Database::for_key(storage_key)) {
        if (!database || database->version() == 0)
            continue;

        rows.append(InspectionDatabaseRow { database->name(), database->version(), database->object_stores().size() });
    }
}

static void append_object_store_rows(Vector<InspectionObject>& rows, StorageAPI::StorageKey const& storage_key, String const& database_name)
{
    auto database = Database::for_key_and_name(storage_key, database_name);
    if (!database.has_value())
        return;

    for (auto const& object_store : database->object_stores())
        rows.append(inspect_object_store(*object_store));
}

static void append_record_rows(Vector<InspectionObject>& rows, DOM::Document& document, StorageAPI::StorageKey const& storage_key, InspectionPath const& path)
{
    if (!path.object_store_name.has_value())
        return;

    auto database = Database::for_key_and_name(storage_key, path.database_name);
    if (!database.has_value())
        return;

    auto object_store = database->object_store_with_name(*path.object_store_name);
    if (!object_store)
        return;

    for (auto const& record : object_store->records()) {
        auto serialized_record_key = serialize_key_for_inspection(*record.key);
        if (path.key.has_value() && serialized_record_key.serialized() != path.key->serialized())
            continue;
        rows.append(InspectionRecord { serialized_record_key, serialize_record_value(document.realm(), *record.value) });
    }
}

Vector<InspectionObject> inspect_indexed_database_objects(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, Optional<Vector<InspectionPath>> const& paths)
{
    Vector<InspectionObject> rows;
    for (auto const& entry : indexed_database_documents_for_inspection(document)) {
        if (!document_matches(entry.document->url()))
            continue;

        if (!paths.has_value()) {
            append_database_rows(rows, entry.storage_key);
            continue;
        }

        for (auto const& path : *paths) {
            if (!path.object_store_name.has_value())
                append_object_store_rows(rows, entry.storage_key, path.database_name);
            else
                append_record_rows(rows, entry.document, entry.storage_key, path);
        }
    }

    return rows;
}

ErrorOr<bool> delete_indexed_database_for_inspection(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, String const& database_name)
{
    auto entry = matching_document(document, document_matches);
    if (!entry.has_value())
        return Error::from_string_literal("Unable to find IndexedDB host");

    auto database = Database::for_key_and_name(entry->storage_key, database_name);
    if (database.has_value() && !database->associated_connections_as_root_vector().is_empty())
        return true;

    TRY(Database::delete_for_key_and_name(entry->storage_key, database_name));
    return false;
}

ErrorOr<void> clear_indexed_database_object_store_for_inspection(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name)
{
    auto entry = matching_document(document, document_matches);
    if (!entry.has_value())
        return Error::from_string_literal("Unable to find IndexedDB host");

    auto database = Database::for_key_and_name(entry->storage_key, database_name);
    if (!database.has_value())
        return Error::from_string_literal("Unable to find IndexedDB database");

    auto object_store = database->object_store_with_name(object_store_name);
    if (!object_store)
        return Error::from_string_literal("Unable to find IndexedDB object store");

    clear_an_object_store(*object_store);
    return {};
}

ErrorOr<void> delete_indexed_database_record_for_inspection(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name, JsonValue const& key)
{
    auto entry = matching_document(document, document_matches);
    if (!entry.has_value())
        return Error::from_string_literal("Unable to find IndexedDB host");

    auto database = Database::for_key_and_name(entry->storage_key, database_name);
    if (!database.has_value())
        return Error::from_string_literal("Unable to find IndexedDB database");

    auto object_store = database->object_store_with_name(object_store_name);
    if (!object_store)
        return Error::from_string_literal("Unable to find IndexedDB object store");

    for (auto const& record : object_store->records()) {
        if (serialize_key_for_inspection(*record.key).serialized() != key.serialized())
            continue;

        auto range = IDBKeyRange::create(entry->document->realm(), record.key, record.key, IDBKeyRange::LowerOpen::No, IDBKeyRange::UpperOpen::No);
        delete_records_from_an_object_store(*object_store, range);
        return {};
    }

    return Error::from_string_literal("Unable to find IndexedDB record");
}

}
