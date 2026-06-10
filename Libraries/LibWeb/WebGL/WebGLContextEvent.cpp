/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLContextEvent);

GC::Ref<WebGLContextEvent> WebGLContextEvent::create(FlyString const& event_name, WebGLContextEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<WebGLContextEvent>(event_name, event_init, time_stamp);
}

WebGLContextEvent::WebGLContextEvent(FlyString const& type, WebGLContextEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(type, event_init, time_stamp)
    , m_status_message(event_init.status_message)
{
}

WebGLContextEvent::~WebGLContextEvent() = default;

}
