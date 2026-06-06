/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/ProgressEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/XHR/ProgressEvent.h>

namespace Web::XHR {

GC_DEFINE_ALLOCATOR(ProgressEvent);

GC::Ref<ProgressEvent> ProgressEvent::create(FlyString const& event_name, Bindings::ProgressEventInit const& event_init,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<ProgressEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<ProgressEvent>> ProgressEvent::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::ProgressEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

ProgressEvent::ProgressEvent(FlyString const& event_name, Bindings::ProgressEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : Event(event_name, event_init, time_stamp)
    , m_length_computable(event_init.length_computable)
    , m_loaded(event_init.loaded)
    , m_total(event_init.total)
{
}

ProgressEvent::~ProgressEvent() = default;

}
