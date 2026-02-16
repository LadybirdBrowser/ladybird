/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrackEventPrototype.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TrackEvent.h>
#include <LibWeb/HTML/VideoTrack.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TrackEvent);

GC::Ref<TrackEvent> TrackEvent::create(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
{
    return realm.create<TrackEvent>(realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<GC::Ref<TrackEvent>> TrackEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

TrackEvent::TrackTypeInternal TrackEvent::to_track_type_internal(TrackEventInit::TrackType const& track_type)
{
    if (!track_type.has_value())
        return Empty {};

    return track_type->visit(
        [](GC::Root<VideoTrack> const& root) -> TrackTypeInternal { return GC::Ref { *root }; },
        [](GC::Root<AudioTrack> const& root) -> TrackTypeInternal { return GC::Ref { *root }; },
        [](GC::Root<TextTrack> const& root) -> TrackTypeInternal { return GC::Ref { *root }; });
}

TrackEvent::TrackEvent(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_track(to_track_type_internal(event_init.track))
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
    m_track.visit(
        [](Empty) {},
        [&](auto const& ref) { visitor.visit(ref); });
}

TrackEvent::TrackReturnType TrackEvent::track() const
{
    return m_track.visit(
        [](Empty) -> TrackReturnType { return Empty {}; },
        [](auto const& ref) -> TrackReturnType { return GC::Root { *ref }; });
}

}
