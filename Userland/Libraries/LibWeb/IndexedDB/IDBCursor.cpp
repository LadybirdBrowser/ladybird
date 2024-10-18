/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBCursorPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBCursor.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBCursor);

IDBCursor::IDBCursor(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

IDBCursor::~IDBCursor() = default;

void IDBCursor::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBCursor);
}

}
