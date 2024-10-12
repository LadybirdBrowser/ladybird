/*
 * Copyright (c) 2024, Brandon Gutzmann <brandgutz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/IDBKeyRangePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/IndexedDB/IDBKeyRange.h>

namespace Web::IndexedDB {

JS_DEFINE_ALLOCATOR(IDBKeyRange);

IDBKeyRange::IDBKeyRange(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

IDBKeyRange::~IDBKeyRange() = default;

void IDBKeyRange::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(IDBKeyRange);
}

}
