/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/IDBVersionChangeEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBVersionChangeEvent.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBVersionChangeEvent);

GC::Ref<IDBVersionChangeEvent> IDBVersionChangeEvent::create(JS::Realm& realm, FlyString const& event_name, IDBVersionChangeEventInit const& event_init)
{
    return realm.create<IDBVersionChangeEvent>(realm, event_name, event_init);
}

IDBVersionChangeEvent::IDBVersionChangeEvent(JS::Realm& realm, FlyString const& event_name, IDBVersionChangeEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_old_version(event_init.old_version)
    , m_new_version(event_init.new_version)
{
}

IDBVersionChangeEvent::~IDBVersionChangeEvent() = default;

void IDBVersionChangeEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBVersionChangeEvent);
}

}
