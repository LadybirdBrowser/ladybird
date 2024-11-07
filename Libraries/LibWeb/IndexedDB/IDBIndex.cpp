/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBIndexPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/IndexedDB/IDBIndex.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBIndex);

IDBIndex::~IDBIndex() = default;

IDBIndex::IDBIndex(JS::Realm& realm)
    : PlatformObject(realm)
{
}

GC::Ref<IDBIndex> IDBIndex::create(JS::Realm& realm)
{
    return realm.create<IDBIndex>(realm);
}

void IDBIndex::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBIndex);
}

}
