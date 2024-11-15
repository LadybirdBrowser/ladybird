/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PromiseRejectionEventPrototype.h>
#include <LibWeb/HTML/PromiseRejectionEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(PromiseRejectionEvent);

GC::Ref<PromiseRejectionEvent> PromiseRejectionEvent::create(JS::Realm& realm, FlyString const& event_name, PromiseRejectionEventInit const& event_init)
{
    return realm.create<PromiseRejectionEvent>(realm, event_name, event_init);
}

WebIDL::ExceptionOr<GC::Ref<PromiseRejectionEvent>> PromiseRejectionEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, PromiseRejectionEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

PromiseRejectionEvent::PromiseRejectionEvent(JS::Realm& realm, FlyString const& event_name, PromiseRejectionEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_promise(*event_init.promise)
    , m_reason(event_init.reason)
{
}

PromiseRejectionEvent::~PromiseRejectionEvent() = default;

void PromiseRejectionEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_promise);
    visitor.visit(m_reason);
}

void PromiseRejectionEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(PromiseRejectionEvent);
}

}
