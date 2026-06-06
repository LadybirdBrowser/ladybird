/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WebGLContextEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLContextEvent);

GC::Ref<WebGLContextEvent> WebGLContextEvent::create(FlyString const& event_name, Bindings::WebGLContextEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<WebGLContextEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<WebGLContextEvent>> WebGLContextEvent::construct_impl(HTML::WindowOrWorkerGlobalScopeMixin& global_scope, FlyString const& event_name, Bindings::WebGLContextEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(global_scope)));
}

WebGLContextEvent::WebGLContextEvent(FlyString const& type, Bindings::WebGLContextEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_status_message(event_init.status_message)
{
}

WebGLContextEvent::~WebGLContextEvent() = default;

}
