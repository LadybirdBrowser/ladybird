/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBRecordPrototype.h>
#include <LibWeb/IndexedDB/IDBRecord.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRecord);

IDBRecord::~IDBRecord() = default;

GC::Ref<IDBRecord> IDBRecord::create(JS::Realm& realm, GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key)
{
    return realm.create<IDBRecord>(realm, move(key), move(value), move(primary_key));
}

IDBRecord::IDBRecord(JS::Realm& realm, GC::Ref<Key> key, JS::Value value, GC::Ref<Key> primary_key)
    : PlatformObject(realm)
    , m_key(move(key))
    , m_value(move(value))
    , m_primary_key(move(primary_key))
{
}

void IDBRecord::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBRecord);
    Base::initialize(realm);
}

void IDBRecord::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_key);
    visitor.visit(m_value);
    visitor.visit(m_primary_key);
}

}
