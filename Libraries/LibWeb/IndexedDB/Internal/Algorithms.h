/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/Key.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::IndexedDB {

WebIDL::ExceptionOr<GC::Ref<IDBDatabase>> open_a_database_connection(JS::Realm&, StorageAPI::StorageKey, String, Optional<u64>, GC::Ref<IDBRequest>);
bool fire_a_version_change_event(JS::Realm&, FlyString const&, GC::Ref<DOM::EventTarget>, u64, Optional<u64>);
ErrorOr<Key> convert_a_value_to_a_key(JS::Realm&, JS::Value, Vector<JS::Value> = {});
void close_a_database_connection(IDBDatabase&, bool forced = false);
void upgrade_a_database(JS::Realm&, GC::Ref<IDBDatabase>, u64, GC::Ref<IDBRequest>);

}
