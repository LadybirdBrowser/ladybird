/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CookieStore/CookieStore.h>

namespace Web::CookieStore {

GC_DEFINE_ALLOCATOR(CookieStore);

CookieStore::CookieStore(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void CookieStore::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CookieStore);
    Base::initialize(realm);
}

}
