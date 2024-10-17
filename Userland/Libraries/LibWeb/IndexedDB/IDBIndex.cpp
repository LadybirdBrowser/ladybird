/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBIndexPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBIndex.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBIndex);

IDBIndex::IDBIndex(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

IDBIndex::~IDBIndex() = default;

void IDBIndex::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBIndex);
}

}
