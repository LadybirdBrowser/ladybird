/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrackEvent.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TrackEvent.h>
#include <LibWeb/HTML/VideoTrack.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TrackEvent);

GC::Ref<TrackEvent> TrackEvent::create(JS::Realm& realm, FlyString const& event_name, Bindings::TrackEventInit const& event_init)
{
    return realm.create<TrackEvent>(realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<GC::Ref<TrackEvent>> TrackEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, Bindings::TrackEventInit const& event_init)
{
    return create(realm, event_name, move(event_init));
}

TrackEvent::TrackEvent(JS::Realm& realm, FlyString const& event_name, Bindings::TrackEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_track(event_init.track)
{
}

void TrackEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrackEvent);
    Base::initialize(realm);
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
