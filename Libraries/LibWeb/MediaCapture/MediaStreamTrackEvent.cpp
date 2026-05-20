/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaStreamTrackEvent.h>
#include <LibWeb/MediaCapture/MediaStreamTrackEvent.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaStreamTrackEvent);

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::create(JS::Realm& realm, FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init)
{
    return realm.create<MediaStreamTrackEvent>(realm, event_name, event_init);
}

GC::Ref<MediaStreamTrackEvent> MediaStreamTrackEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

// https://w3c.github.io/mediacapture-main/#mediastreamtrackevent
MediaStreamTrackEvent::MediaStreamTrackEvent(JS::Realm& realm, FlyString const& event_name, Bindings::MediaStreamTrackEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_track(event_init.track)
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
