/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/HashChangeEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HashChangeEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HashChangeEvent);

[[nodiscard]] JS::NonnullGCPtr<HashChangeEvent> HashChangeEvent::create(JS::Realm& realm, FlyString const& event_name, HashChangeEventInit const& event_init)
{
    return realm.heap().allocate<HashChangeEvent>(realm, realm, event_name, event_init);
}

JS::NonnullGCPtr<HashChangeEvent> HashChangeEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, HashChangeEventInit const& event_init)
{
    return realm.heap().allocate<HashChangeEvent>(realm, realm, event_name, event_init);
}

HashChangeEvent::HashChangeEvent(JS::Realm& realm, FlyString const& event_name, HashChangeEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_old_url(event_init.old_url)
    , m_new_url(event_init.new_url)
{
}

void HashChangeEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HashChangeEvent);
}

void HashChangeEvent::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
