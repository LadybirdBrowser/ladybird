/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/CloseEvent.h>
#include <LibWeb/HTML/CloseEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CloseEvent);

GC::Ref<CloseEvent> CloseEvent::create(FlyString const& event_name, Bindings::CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CloseEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<CloseEvent>> CloseEvent::construct_impl(WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::CloseEventInit const& event_init)
{
    return GC::Heap::the().allocate<CloseEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(relevant_global_object(global_scope)));
}

CloseEvent::CloseEvent(FlyString const& event_name, Bindings::CloseEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_was_clean(event_init.was_clean)
    , m_code(event_init.code)
    , m_reason(event_init.reason)
{
}

CloseEvent::~CloseEvent() = default;

}
