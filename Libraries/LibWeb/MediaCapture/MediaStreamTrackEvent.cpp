/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaStreamTrackEventPrototype.h>
#include <LibWeb/MediaCapture/MediaStreamTrackEvent.h>

namespace Web::MediaCapture {

static GC::Ref<MediaStreamTrack> require_track(MediaStreamTrackEventInit const& event_init)
{
    VERIFY(event_init.track);
    return *event_init.track;
}

GC_DEFINE_ALLOCATOR(MediaStreamTrackEvent);

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::create(JS::Realm& realm, FlyString const& event_name, MediaStreamTrackEventInit const& event_init)
{
    return realm.create<MediaStreamTrackEvent>(realm, event_name, event_init);
}

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, MediaStreamTrackEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

MediaStreamTrackEvent::MediaStreamTrackEvent(JS::Realm& realm, FlyString const& event_name, MediaStreamTrackEventInit const& event_init)
    : DOM::Event(realm, event_name, [&] {
        DOM::EventInit base_init {};
        base_init.bubbles = event_init.bubbles;
        base_init.cancelable = event_init.cancelable;
        base_init.composed = event_init.composed;
        return base_init;
    }())
    , m_track(require_track(event_init))
{
}

MediaStreamTrackEvent::~MediaStreamTrackEvent() = default;

void MediaStreamTrackEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaStreamTrackEvent);
    Base::initialize(realm);
}

void MediaStreamTrackEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_track);
}

}
