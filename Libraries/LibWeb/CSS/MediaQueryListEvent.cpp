/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/MediaQueryListEvent.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaQueryListEvent);

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create(
    FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<MediaQueryListEvent>(event_name, event_init, time_stamp);
    event->set_is_trusted(true);
    return event;
}

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init)
{
    return GC::Heap::the().allocate<MediaQueryListEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

MediaQueryListEvent::MediaQueryListEvent(FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_media(event_init.media)
    , m_matches(event_init.matches)
{
}

MediaQueryListEvent::~MediaQueryListEvent() = default;

}
