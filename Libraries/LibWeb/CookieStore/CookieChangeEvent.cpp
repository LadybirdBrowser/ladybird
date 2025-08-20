/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CookieChangeEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CookieStore/CookieChangeEvent.h>

namespace Web::CookieStore {

GC_DEFINE_ALLOCATOR(CookieChangeEvent);

GC::Ref<CookieChangeEvent> CookieChangeEvent::create(JS::Realm& realm, FlyString const& event_name, CookieChangeEventInit const& event_init)
{
    return realm.create<CookieChangeEvent>(realm, event_name, event_init);
}

GC::Ref<CookieChangeEvent> CookieChangeEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, CookieChangeEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

CookieChangeEvent::CookieChangeEvent(JS::Realm& realm, FlyString const& event_name, CookieChangeEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_changed(event_init.changed.value_or({}))
    , m_deleted(event_init.deleted.value_or({}))
{
}

CookieChangeEvent::~CookieChangeEvent() = default;

void CookieChangeEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CookieChangeEvent);
    Base::initialize(realm);
}

void CookieChangeEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    VISIT_CACHED_ATTRIBUTE(changed);
    VISIT_CACHED_ATTRIBUTE(deleted);
}

}
