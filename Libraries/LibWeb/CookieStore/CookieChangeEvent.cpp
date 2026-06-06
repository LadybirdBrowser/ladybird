/*
 * Copyright (c) 2025, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/CookieChangeEvent.h>
#include <LibWeb/CookieStore/CookieChangeEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::CookieStore {

GC_DEFINE_ALLOCATOR(CookieChangeEvent);

GC::Ref<CookieChangeEvent> CookieChangeEvent::create(FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CookieChangeEvent>(event_name, event_init, time_stamp);
}

GC::Ref<CookieChangeEvent> CookieChangeEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

CookieChangeEvent::CookieChangeEvent(FlyString const& event_name, Bindings::CookieChangeEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_changed(event_init.changed.value_or({}))
    , m_deleted(event_init.deleted.value_or({}))
{
}

CookieChangeEvent::~CookieChangeEvent() = default;

void CookieChangeEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    VISIT_CACHED_ATTRIBUTE(changed);
    VISIT_CACHED_ATTRIBUTE(deleted);
}

}
