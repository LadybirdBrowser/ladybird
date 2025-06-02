/*
 * Copyright (c) 2025, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CookieStorePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CookieStore.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CookieStore);

GC::Ref<CookieStore> CookieStore::create(JS::Realm& realm, GC::Ref<Page> page)
{
    return realm.create<CookieStore>(realm, page);
}

CookieStore::CookieStore(JS::Realm& realm, GC::Ref<Page> page)
    : DOM::EventTarget(realm)
    , m_page(page)
{
}

void CookieStore::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CookieStore);
    Base::initialize(realm);
}

void CookieStore::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
}

}
