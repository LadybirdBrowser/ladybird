/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/MediaQueryList.h>
#include <LibWeb/CSS/MediaQueryListEvent.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(MediaQueryListEvent);

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create(
    FlyString const& event_name, MediaQueryListEventInit const& event_init,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<MediaQueryListEvent>(event_name, event_init, time_stamp);
}

GC::Ref<MediaQueryListEvent> MediaQueryListEvent::create(
    FlyString const& event_name, String media, bool matches,
    HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<MediaQueryListEvent>(event_name, move(media), matches, time_stamp);
    event->set_is_trusted(true);
    return event;
}

MediaQueryListEvent::MediaQueryListEvent(FlyString const& event_name, MediaQueryListEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_media(event_init.media)
    , m_matches(event_init.matches)
{
}

MediaQueryListEvent::MediaQueryListEvent(FlyString const& event_name, String media, bool matches, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, time_stamp)
    , m_media(move(media))
    , m_matches(matches)
{
}

MediaQueryListEvent::~MediaQueryListEvent() = default;

}

namespace Web::Bindings {

JS::Realm& wrapper_realm_for_media_query_list_event(WrapperWorld const& wrapper_world, JS::Realm& preferred_realm, CSS::MediaQueryListEvent& event)
{
    if (!wrapper_world.is_main_world())
        return preferred_realm;

    if (auto target = event.target()) {
        if (auto* media_query_list = as_if<CSS::MediaQueryList>(target.ptr()))
            return wrapper_realm_for_media_query_list(wrapper_world, preferred_realm, *media_query_list);
    }

    return preferred_realm;
}

}
