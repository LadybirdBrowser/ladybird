/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ErrorEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/ErrorEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(ErrorEvent);

GC::Ref<ErrorEvent> ErrorEvent::create(JS::Realm& realm, FlyString const& event_name, ErrorEventInit const& event_init)
{
    auto event = realm.create<ErrorEvent>(realm, event_name, event_init);
    event->set_is_trusted(true);
    return event;
}

WebIDL::ExceptionOr<GC::Ref<ErrorEvent>> ErrorEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, ErrorEventInit const& event_init)
{
    return realm.create<ErrorEvent>(realm, event_name, event_init);
}

ErrorEvent::ErrorEvent(JS::Realm& realm, FlyString const& event_name, ErrorEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_message(event_init.message)
    , m_filename(event_init.filename)
    , m_lineno(event_init.lineno)
    , m_colno(event_init.colno)
    , m_error(event_init.error)
{
}

ErrorEvent::~ErrorEvent() = default;

void ErrorEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ErrorEvent);
}

void ErrorEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_error);
}

}
