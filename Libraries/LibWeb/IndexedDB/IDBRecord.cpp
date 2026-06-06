/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/IndexedDB/IDBRecord.h>
#include <LibWeb/IndexedDB/Internal/Algorithms.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRecord);

IDBRecord::~IDBRecord() = default;

GC::Ref<IDBRecord> IDBRecord::create(GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key)
{
    return GC::Heap::the().allocate<IDBRecord>(move(key), move(value), move(primary_key));
}

IDBRecord::IDBRecord(GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key)
    : Bindings::Wrappable()
    , m_key(move(key))
    , m_value(move(value))
    , m_primary_key(move(primary_key))
{
}

void IDBRecord::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_key);
    visitor.visit(m_value);
    visitor.visit(m_primary_key);
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#dom-idbrecord-key
WebIDL::ExceptionOr<JS::Value> IDBRecord::key(JS::Realm& realm) const
{
    // The key getter steps are to return the result of converting a key to a value with this’s key.
    return convert_a_key_to_a_value(realm, m_key);
}

// https://pr-preview.s3.amazonaws.com/w3c/IndexedDB/pull/461.html#dom-idbrecord-primarykey
WebIDL::ExceptionOr<JS::Value> IDBRecord::primary_key(JS::Realm& realm) const
{
    // The primaryKey getter steps are to return the result of converting a key to a value with this’s primary key.
    return convert_a_key_to_a_value(realm, m_primary_key);
}

}
