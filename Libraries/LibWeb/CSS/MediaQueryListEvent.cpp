/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaQueryListEvent);

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MediaQueryListEvent>(event_name, event_init, time_stamp);
}

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create(
    Utf16FlyString const& event_name, Utf16String media, bool matches,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<MediaQueryListEvent>(event_name, move(media), matches, time_stamp);
    event->set_is_trusted(true);
    return event;
}

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create_for_constructor(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return create(event_name, event_init, time_stamp);
}

MediaQueryListEvent::MediaQueryListEvent(Utf16FlyString const& event_name, Bindings::MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { event_name.to_utf16_string().to_utf8() }, event_init, time_stamp)
    , m_media(event_init.media)
    , m_matches(event_init.matches)
{
}

MediaQueryListEvent::MediaQueryListEvent(Utf16FlyString const& event_name, Utf16String media, bool matches, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(FlyString { event_name.to_utf16_string().to_utf8() }, time_stamp)
    , m_media(move(media))
    , m_matches(matches)
{
}

MediaQueryListEvent::~MediaQueryListEvent() = default;

}
