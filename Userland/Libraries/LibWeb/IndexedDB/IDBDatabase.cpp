/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBDatabasePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBDatabase.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBDatabase);

IDBDatabase::~IDBDatabase() = default;

IDBDatabase::IDBDatabase(JS::Realm& realm)
    : EventTarget(realm)
{
}

void IDBDatabase::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBDatabase);
}

}
