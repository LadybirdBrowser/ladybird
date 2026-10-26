/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibJS/HostClassBuilder.h>
#include <LibURL/RustIntegration.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/IndexedDB/Inspection.h>
#include <LibWeb/Infra/JSON.h>
#include <LibWeb/StorageAPI/StorageKey.h>

// The inspection entry points of the Rust implementation of IndexedDB, Libraries/LibWeb/Bindings/Rust/src/indexed_db/exports.rs.
extern "C" {

struct KeyPathAbi {
    u8 kind;
    FlatPtr const* strings;
    size_t count;
};

enum class InspectionStatus : u8 {
    Done = 0,
    NoDatabase = 1,
    NoObjectStore = 2,
    NoRecord = 3,
    Blocked = 4,
};

void web_rust_indexed_db_inspect_databases(JSVM*, URL::FFI::FfiOrigin const*, void* ctx, void (*on_database)(void*, FlatPtr name, u64 version, FlatPtr const* object_store_names, size_t count));
bool web_rust_indexed_db_inspect_object_stores(JSVM*, URL::FFI::FfiOrigin const*, FlatPtr database_name, void* ctx, void (*on_object_store)(void*, FlatPtr name, KeyPathAbi const*, bool auto_increment), void (*on_index)(void*, FlatPtr name, KeyPathAbi const*, bool unique, bool multi_entry));
void web_rust_indexed_db_inspect_records(JSVM*, URL::FFI::FfiOrigin const*, FlatPtr database_name, FlatPtr object_store_name, void* ctx, void (*on_record)(void*, JsonValue* key, ReadonlyBytes value));
InspectionStatus web_rust_indexed_db_delete_database_for_inspection(JSVM*, URL::FFI::FfiOrigin const*, FlatPtr database_name);
InspectionStatus web_rust_indexed_db_clear_object_store_for_inspection(JSVM*, URL::FFI::FfiOrigin const*, FlatPtr database_name, FlatPtr object_store_name);
InspectionStatus web_rust_indexed_db_delete_record_for_inspection(JSVM*, URL::FFI::FfiOrigin const*, FlatPtr database_name, FlatPtr object_store_name, void* ctx, bool (*matches)(void*, JsonValue* key));
}

namespace Web::IndexedDB {

struct IndexedDatabaseDocument {
    String url;
    GC::Ref<DOM::Document> document;
    StorageAPI::StorageKey storage_key;
};

static JSVM* vm()
{
    return JS::HostABI::vm_to_abi(Bindings::main_thread_vm());
}

static String serialize_key_path(KeyPathAbi const& key_path)
{
    if (key_path.kind == 1)
        return Utf16String::from_raw(key_path.strings[0]).to_utf8();

    JsonArray array;
    for (size_t i = 0; i < key_path.count; ++i)
        array.must_append(Utf16String::from_raw(key_path.strings[i]).to_utf8());
    return array.serialized();
}

static String serialize_record_value(JS::Realm& realm, ReadonlyBytes bytes)
{
    HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    HTML::StorageSerializationRecord record { MUST(ByteBuffer::copy(bytes)) };
    auto value = HTML::structured_deserialize(realm.vm(), record, realm);
    if (value.is_exception())
        return "<unserializable>"_string;

    auto serialized = Infra::serialize_javascript_value_to_json_string(realm, value.release_value());
    if (serialized.is_exception())
        return "<unserializable>"_string;

    return serialized.release_value();
}

static GC::ConservativeVector<IndexedDatabaseDocument> indexed_database_documents_for_inspection(DOM::Document& root_document)
{
    GC::ConservativeVector<IndexedDatabaseDocument> documents;

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

        // FIXME: Inspect a document hosted by another process.
        auto* local_navigable = as_if<HTML::LocalNavigable>(*content_navigable);
        if (!local_navigable)
            return TraversalDecision::Continue;
        auto content_document = local_navigable->active_document();
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

template<typename Callback>
static auto with_origin(StorageAPI::StorageKey const& storage_key, Callback callback)
{
    using Result = decltype(callback(declval<URL::FFI::FfiOrigin const*>()));
    if constexpr (IsSame<Result, void>) {
        URL::RustIntegration::lend_origin_to_ffi(storage_key.origin, [&](URL::FFI::FfiOrigin const& origin) { callback(&origin); });
    } else {
        Optional<Result> result;
        URL::RustIntegration::lend_origin_to_ffi(storage_key.origin, [&](URL::FFI::FfiOrigin const& origin) { result = callback(&origin); });
        return result.release_value();
    }
}

struct DatabaseVisitor {
    Function<void(String, u64, Vector<String>)> on_database;
};

static void for_each_database(StorageAPI::StorageKey const& storage_key, Function<void(String, u64, Vector<String>)> on_database)
{
    DatabaseVisitor visitor { move(on_database) };
    with_origin(storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        web_rust_indexed_db_inspect_databases(vm(), origin, &visitor, [](void* ctx, FlatPtr name, u64 version, FlatPtr const* object_store_names, size_t count) {
            Vector<String> names;
            for (size_t i = 0; i < count; ++i)
                names.append(Utf16String::from_raw(object_store_names[i]).to_utf8());
            static_cast<DatabaseVisitor*>(ctx)->on_database(Utf16String::from_raw(name).to_utf8(), version, move(names));
        });
    });
}

Vector<InspectionStorageHost> inspect_indexed_database_storage(DOM::Document& document)
{
    Vector<InspectionStorageHost> hosts;
    for (auto const& entry : indexed_database_documents_for_inspection(document)) {
        Vector<InspectionDatabase> databases;
        for_each_database(entry.storage_key, [&](String name, u64 version, Vector<String> object_store_names) {
            if (version == 0)
                return;

            databases.append({ move(name), version, move(object_store_names) });
        });

        hosts.append({ entry.url, move(databases) });
    }
    return hosts;
}

static void append_database_rows(Vector<InspectionObject>& rows, StorageAPI::StorageKey const& storage_key)
{
    for_each_database(storage_key, [&](String name, u64 version, Vector<String> object_store_names) {
        if (version == 0)
            return;

        rows.append(InspectionDatabaseRow { move(name), version, object_store_names.size() });
    });
}

static void append_object_store_rows(Vector<InspectionObject>& rows, StorageAPI::StorageKey const& storage_key, String const& database_name)
{
    auto name = Utf16String::from_utf8(database_name);
    with_origin(storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        web_rust_indexed_db_inspect_object_stores(
            vm(), origin, name.raw_identity(), &rows,
            [](void* ctx, FlatPtr name, KeyPathAbi const* key_path, bool auto_increment) {
                Optional<String> serialized_key_path;
                if (key_path->kind != 0)
                    serialized_key_path = serialize_key_path(*key_path);
                static_cast<Vector<InspectionObject>*>(ctx)->append(InspectionObjectStore { Utf16String::from_raw(name).to_utf8(), move(serialized_key_path), auto_increment, {} });
            },
            [](void* ctx, FlatPtr name, KeyPathAbi const* key_path, bool unique, bool multi_entry) {
                auto& object_store = static_cast<Vector<InspectionObject>*>(ctx)->last().get<InspectionObjectStore>();
                object_store.indexes.append({ Utf16String::from_raw(name).to_utf8(), serialize_key_path(*key_path), unique, multi_entry });
            });
    });
}

struct RecordVisitor {
    Vector<InspectionObject>& rows;
    GC::Ref<JS::Realm> realm;
    InspectionPath const& path;
};

static void append_record_rows(Vector<InspectionObject>& rows, DOM::Document& document, StorageAPI::StorageKey const& storage_key, InspectionPath const& path)
{
    if (!path.object_store_name.has_value())
        return;

    auto database_name = Utf16String::from_utf8(path.database_name);
    auto object_store_name = Utf16String::from_utf8(*path.object_store_name);
    RecordVisitor visitor { rows, HTML::relevant_realm(document), path };
    with_origin(storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        web_rust_indexed_db_inspect_records(vm(), origin, database_name.raw_identity(), object_store_name.raw_identity(), &visitor, [](void* ctx, JsonValue* key, ReadonlyBytes value) {
            auto& visitor = *static_cast<RecordVisitor*>(ctx);
            auto serialized_record_key = move(*key);
            delete key;
            if (visitor.path.key.has_value() && serialized_record_key.serialized() != visitor.path.key->serialized())
                return;
            visitor.rows.append(InspectionRecord { move(serialized_record_key), serialize_record_value(visitor.realm, value) });
        });
    });
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

    auto name = Utf16String::from_utf8(database_name);
    auto status = with_origin(entry->storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        return web_rust_indexed_db_delete_database_for_inspection(vm(), origin, name.raw_identity());
    });
    return status == InspectionStatus::Blocked;
}

static ErrorOr<void> error_for_status(InspectionStatus status)
{
    switch (status) {
    case InspectionStatus::Done:
        return {};
    case InspectionStatus::NoDatabase:
        return Error::from_string_literal("Unable to find IndexedDB database");
    case InspectionStatus::NoObjectStore:
        return Error::from_string_literal("Unable to find IndexedDB object store");
    case InspectionStatus::NoRecord:
        return Error::from_string_literal("Unable to find IndexedDB record");
    case InspectionStatus::Blocked:
        break;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<void> clear_indexed_database_object_store_for_inspection(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name)
{
    auto entry = matching_document(document, document_matches);
    if (!entry.has_value())
        return Error::from_string_literal("Unable to find IndexedDB host");

    auto database = Utf16String::from_utf8(database_name);
    auto object_store = Utf16String::from_utf8(object_store_name);
    auto status = with_origin(entry->storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        return web_rust_indexed_db_clear_object_store_for_inspection(vm(), origin, database.raw_identity(), object_store.raw_identity());
    });
    return error_for_status(status);
}

ErrorOr<void> delete_indexed_database_record_for_inspection(DOM::Document& document, Function<bool(URL::URL const&)> const& document_matches, String const& database_name, String const& object_store_name, JsonValue const& key)
{
    auto entry = matching_document(document, document_matches);
    if (!entry.has_value())
        return Error::from_string_literal("Unable to find IndexedDB host");

    auto database = Utf16String::from_utf8(database_name);
    auto object_store = Utf16String::from_utf8(object_store_name);
    auto serialized_key = key.serialized();
    auto status = with_origin(entry->storage_key, [&](URL::FFI::FfiOrigin const* origin) {
        return web_rust_indexed_db_delete_record_for_inspection(vm(), origin, database.raw_identity(), object_store.raw_identity(), &serialized_key, [](void* ctx, JsonValue* record_key) {
            auto serialized_record_key = move(*record_key);
            delete record_key;
            return serialized_record_key.serialized() == *static_cast<String*>(ctx);
        });
    });
    return error_for_status(status);
}

}
