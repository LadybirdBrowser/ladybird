/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Variant.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/IDBCursor.h>
#include <LibWeb/Bindings/IDBObjectStore.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/StorageAPI/StorageKey.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::IndexedDB {

class IDBCursor;

enum class RecordKind {
    Key,
    Value,
    Record
};

using KeyPath = Variant<String, Vector<String>>;
using RecordSource = Variant<GC::Ref<ObjectStore>, GC::Ref<Index>>;

using RetrieveMultipleItemsOptions = Bindings::IDBGetAllOptions;

void open_a_database_connection(JS::Realm&, StorageAPI::StorageKey, String, Optional<u64>, GC::Ref<IDBRequest>, GC::Ref<GC::Function<void(WebIDL::ExceptionOr<GC::Ref<IDBDatabase>>)>>);
bool fire_a_version_change_event(JS::Realm&, FlyString const&, GC::Ref<DOM::EventTarget>, u64, Optional<u64>);
WebIDL::ExceptionOr<GC::Ref<Key>> convert_a_value_to_a_key(JS::Realm&, JS::Value, Vector<JS::Value> = {});
void close_a_database_connection(GC::Ref<IDBDatabase>, GC::Ptr<GC::Function<void()>> on_complete = nullptr, bool forced = false);
void upgrade_a_database(JS::Realm&, GC::Ref<IDBDatabase>, u64, GC::Ref<IDBRequest>, GC::Ref<GC::Function<void()>>);
void delete_a_database(JS::Realm&, StorageAPI::StorageKey, String, GC::Ref<IDBRequest>, GC::Ref<GC::Function<void(WebIDL::ExceptionOr<u64>)>>);
void abort_a_transaction(GC::Ref<IDBTransaction>, GC::Ptr<WebIDL::DOMException>);
JS::Value convert_a_key_to_a_value(JS::Realm&, GC::Ref<Key>);
JS::ThrowCompletionOr<JS::Value> convert_a_key_path_to_a_value(JS::Realm&, KeyPath const&);
bool is_valid_key_path(KeyPath const&);
GC::Ref<HTML::DOMStringList> create_a_sorted_name_list(Vector<String>);
void commit_a_transaction(JS::Realm&, GC::Ref<IDBTransaction>);
WebIDL::ExceptionOr<JS::Value> clone_in_realm(JS::Realm&, JS::Value, GC::Ref<IDBTransaction>);
WebIDL::ExceptionOr<GC::Ref<Key>> convert_a_value_to_a_multi_entry_key(JS::Realm&, JS::Value);
WebIDL::ExceptionOr<ErrorOr<JS::Value>> evaluate_key_path_on_a_value(JS::Realm&, JS::Value, KeyPath const&);
WebIDL::ExceptionOr<ErrorOr<GC::Ref<Key>>> extract_a_key_from_a_value_using_a_key_path(JS::Realm&, JS::Value, KeyPath const&, bool = false);
bool check_that_a_key_could_be_injected_into_a_value(JS::Realm&, JS::Value, KeyPath const&);
void fire_an_error_event(JS::Realm&, GC::Ref<IDBRequest>);
void fire_a_success_event(JS::Realm&, GC::Ref<IDBRequest>);
GC::Ref<IDBRequest> asynchronously_execute_a_request(JS::Realm&, IDBRequestSource, GC::Ref<GC::Function<WebIDL::ExceptionOr<JS::Value>()>>, GC::Ptr<IDBRequest> = nullptr);
void inject_a_key_into_a_value_using_a_key_path(JS::Realm&, JS::Value, GC::Ref<Key>, KeyPath const&);
JS::Value delete_records_from_an_object_store(GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>);
WebIDL::ExceptionOr<GC::Ptr<Key>> store_a_record_into_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, JS::Value, GC::Ptr<Key>, bool);
WebIDL::ExceptionOr<GC::Ref<IDBKeyRange>> convert_a_value_to_a_key_range(JS::Realm&, Optional<JS::Value>, bool = false);
JS::Value count_the_records_in_a_range(RecordSource, GC::Ref<IDBKeyRange>);
WebIDL::ExceptionOr<JS::Value> retrieve_a_value_from_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>);
GC::Ptr<IDBCursor> iterate_a_cursor(JS::Realm&, GC::Ref<IDBCursor>, GC::Ptr<Key> = nullptr, GC::Ptr<Key> = nullptr, u64 = 1);
JS::Value clear_an_object_store(GC::Ref<ObjectStore>);
JS::Value retrieve_a_key_from_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>);
GC::Ref<JS::Array> retrieve_multiple_values_from_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>, Optional<WebIDL::UnsignedLong>);
GC::Ref<JS::Array> retrieve_multiple_keys_from_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>, Optional<WebIDL::UnsignedLong>);
JS::Value retrieve_a_referenced_value_from_an_index(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBKeyRange>);
JS::Value retrieve_a_value_from_an_index(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBKeyRange>);
GC::Ref<JS::Array> retrieve_multiple_referenced_values_from_an_index(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBKeyRange>, Optional<WebIDL::UnsignedLong>);
GC::Ref<JS::Array> retrieve_multiple_values_from_an_index(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBKeyRange>, Optional<WebIDL::UnsignedLong>);
void queue_a_database_task(GC::Ref<GC::Function<void()>>);
bool cleanup_indexed_database_transactions(GC::Ref<HTML::EventLoop>);
bool is_a_potentially_valid_key_range(JS::Realm&, JS::Value);
GC::Ref<JS::Array> retrieve_multiple_items_from_an_object_store(JS::Realm&, GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>, RecordKind, CursorDirection, Optional<WebIDL::UnsignedLong>);
GC::Ref<JS::Array> retrieve_multiple_items_from_an_index(JS::Realm&, GC::Ref<Index>, GC::Ref<IDBKeyRange>, RecordKind, CursorDirection, Optional<WebIDL::UnsignedLong>);
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> create_a_request_to_retrieve_multiple_items(JS::Realm&, IDBRequestSource, RecordKind, JS::Value, Optional<WebIDL::UnsignedLong>);
WebIDL::ExceptionOr<GC::Ref<IDBRequest>> create_a_request_to_retrieve_multiple_items(JS::Realm&, IDBRequestSource, RecordKind, RetrieveMultipleItemsOptions const&);

}

namespace Web::Bindings {

WEB_API void set_idb_request_result(JS::Realm&, IndexedDB::IDBRequest&, GC::Ref<IndexedDB::IDBDatabase>);
WEB_API void resolve_database_info_list_promise(JS::Realm&, WebIDL::Promise const&, Vector<GC::Weak<IndexedDB::Database>> const&);
WEB_API void append_idb_record(JS::Realm&, JS::Array&, u32, GC::Ref<IndexedDB::IDBRecord>);
WEB_API JS::Value idb_cursor_result(JS::Realm&, GC::Ptr<IndexedDB::IDBCursor>);
WEB_API Optional<JS::Value> key_path_value_for_blob_or_file(JS::Realm&, JS::Value, StringView);
WEB_API GC::Ptr<IndexedDB::IDBKeyRange> idb_key_range_from_value(JS::Value);

}
