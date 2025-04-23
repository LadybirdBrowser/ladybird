/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/DOMStringList.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

using KeyPath = Variant<String, Vector<String>>;

WebIDL::ExceptionOr<GC::Ref<IDBDatabase>> open_a_database_connection(JS::Realm&, StorageAPI::StorageKey, String, Optional<u64>, GC::Ref<IDBRequest>);
bool fire_a_version_change_event(JS::Realm&, FlyString const&, GC::Ref<DOM::EventTarget>, u64, Optional<u64>);
WebIDL::ExceptionOr<ErrorOr<GC::Ref<Key>>> convert_a_value_to_a_key(JS::Realm&, JS::Value, Vector<JS::Value> = {});
void close_a_database_connection(GC::Ref<IDBDatabase>, bool forced = false);
GC::Ref<IDBTransaction> upgrade_a_database(JS::Realm&, GC::Ref<IDBDatabase>, u64, GC::Ref<IDBRequest>);
WebIDL::ExceptionOr<u64> delete_a_database(JS::Realm&, StorageAPI::StorageKey, String, GC::Ref<IDBRequest>);
void abort_a_transaction(GC::Ref<IDBTransaction>, GC::Ptr<WebIDL::DOMException>);
JS::Value convert_a_key_to_a_value(JS::Realm&, GC::Ref<Key>);
bool is_valid_key_path(KeyPath const&);
GC::Ref<HTML::DOMStringList> create_a_sorted_name_list(JS::Realm&, Vector<String>);
void commit_a_transaction(JS::Realm&, GC::Ref<IDBTransaction>);
WebIDL::ExceptionOr<JS::Value> clone_in_realm(JS::Realm&, JS::Value, GC::Ref<IDBTransaction>);
WebIDL::ExceptionOr<ErrorOr<GC::Ref<Key>>> convert_a_value_to_a_multi_entry_key(JS::Realm&, JS::Value);
WebIDL::ExceptionOr<ErrorOr<JS::Value>> evaluate_key_path_on_a_value(JS::Realm&, JS::Value, KeyPath const&);
WebIDL::ExceptionOr<ErrorOr<GC::Ref<Key>>> extract_a_key_from_a_value_using_a_key_path(JS::Realm&, JS::Value, KeyPath const&, bool = false);
bool check_that_a_key_could_be_injected_into_a_value(JS::Realm&, JS::Value, KeyPath const&);
void fire_an_error_event(JS::Realm&, GC::Ref<IDBRequest>);
void fire_a_success_event(JS::Realm&, GC::Ref<IDBRequest>);
GC::Ref<IDBRequest> asynchronously_execute_a_request(JS::Realm&, IDBRequestSource, GC::Ref<GC::Function<WebIDL::ExceptionOr<JS::Value>()>>, GC::Ptr<IDBRequest> = nullptr);
ErrorOr<u64> generate_a_key(GC::Ref<ObjectStore>);
void possibly_update_the_key_generator(GC::Ref<ObjectStore>, GC::Ref<Key>);
void inject_a_key_into_a_value_using_a_key_path(JS::Realm&, JS::Value, GC::Ref<Key>, KeyPath const&);
void delete_records_from_an_object_store(GC::Ref<ObjectStore>, GC::Ref<IDBKeyRange>);

}
