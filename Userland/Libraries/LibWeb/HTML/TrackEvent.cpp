/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/TrackEventPrototype.h>
#include <LibWeb/HTML/TrackEvent.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(TrackEvent);

JS::NonnullGCPtr<TrackEvent> TrackEvent::create(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
{
    return realm.heap().allocate<TrackEvent>(realm, realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<TrackEvent>> TrackEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

TrackEvent::TrackEvent(JS::Realm& realm, FlyString const& event_name, TrackEventInit event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_track(move(event_init.track))
{
}

void TrackEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(TrackEvent);
}

Variant<Empty, JS::Handle<VideoTrack>, JS::Handle<AudioTrack>, JS::Handle<TextTrack>> TrackEvent::track() const
{
    // FIXME: This is a bit awkward. When creating a nullable union, our IDL generator creates a type of
    //        Optional<Variant<...>>, using an empty Optional to represent null. But when retrieving the
    //        attribute, it expects a type of Variant<Empty, ...>, using Empty to represent null.
    if (!m_track.has_value())
        return Empty {};

    return *m_track;
}

}
