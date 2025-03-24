/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/ObjectStore.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(ObjectStore);

ObjectStore::~ObjectStore() = default;

GC::Ref<ObjectStore> ObjectStore::create(JS::Realm& realm, GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path)
{
    return realm.create<ObjectStore>(database, name, auto_increment, key_path);
}

ObjectStore::ObjectStore(GC::Ref<Database> database, String name, bool auto_increment, Optional<KeyPath> const& key_path)
    : m_database(database)
    , m_name(move(name))
    , m_key_path(key_path)
{
    database->add_object_store(*this);

    if (auto_increment)
        m_key_generator = KeyGenerator {};
}

void ObjectStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_database);
}

}
