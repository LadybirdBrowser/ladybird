/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::IndexedDB {

class IDBKeyRange;

}

namespace Web::Bindings {

WEB_API IndexedDB::IDBKeyRange* key_range_from_value(JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> only(JS::Realm&, JS::Value);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> lower_bound(JS::Realm&, JS::Value, bool open);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> upper_bound(JS::Realm&, JS::Value, bool open);
WEB_API WebIDL::ExceptionOr<GC::Ref<IndexedDB::IDBKeyRange>> bound(JS::Realm&, JS::Value lower, JS::Value upper, bool lower_open, bool upper_open);
WEB_API WebIDL::ExceptionOr<bool> includes(JS::Realm&, IndexedDB::IDBKeyRange&, JS::Value key);
WEB_API JS::Value key_range_lower(JS::Realm&, IndexedDB::IDBKeyRange&);
WEB_API JS::Value key_range_upper(JS::Realm&, IndexedDB::IDBKeyRange&);

}
