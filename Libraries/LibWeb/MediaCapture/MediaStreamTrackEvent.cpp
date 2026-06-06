/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/MediaStreamTrackEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaStreamTrackEvent.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStreamTrackEvent);

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::create(FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MediaStreamTrackEvent>(event_name, event_init, time_stamp);
}

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init)
{
    return create(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

MediaStreamTrackEvent::MediaStreamTrackEvent(FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_track(event_init.track)
{
}

MediaStreamTrackEvent::~MediaStreamTrackEvent() = default;

void MediaStreamTrackEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

}
