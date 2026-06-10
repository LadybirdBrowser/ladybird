/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TrackEvent.h>
#include <LibWeb/HTML/VideoTrack.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TrackEvent);

GC::Ref<TrackEvent> TrackEvent::create(FlyString const& event_name, TrackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TrackEvent>(event_name, event_init, time_stamp);
}

TrackEvent::TrackEvent(FlyString const& event_name, TrackEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_track(event_init.track)
{
}

void TrackEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

NullableTrackType TrackEvent::track() const
{
    return m_track;
}

}
