/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/MediaCapture/MediaStreamTrackEvent.h>

namespace Web::MediaCapture {

extern "C" {
// Defined in Bindings/Rust/src/media_capture/media_stream_track_event.rs.
DOM::Event* web_rust_media_stream_track_event_create(JSVM*, void* realm, FlatPtr event_name, bool bubbles, bool cancelable, bool composed, MediaStreamTrack* track);

// Called by Bindings/Rust/src/media_capture/media_stream_track_event.rs.
MediaStreamTrack* web_media_capture_media_stream_track_from_wrapper(JS::Object*);
JS::Object* web_media_capture_media_stream_track_wrap(JS::Realm*, MediaStreamTrack*);
}

GC::Ref<DOM::Event> create_media_stream_track_event(JS::Realm& realm, Utf16FlyString const& event_name, DOM::EventInit const& event_init, MediaStreamTrack& track)
{
    return *web_rust_media_stream_track_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, event_name.raw_identity(), event_init.bubbles, event_init.cancelable, event_init.composed, &track);
}

MediaStreamTrack* web_media_capture_media_stream_track_from_wrapper(JS::Object* object)
{
    return Bindings::impl_from<MediaStreamTrack>(object);
}

JS::Object* web_media_capture_media_stream_track_wrap(JS::Realm* realm, MediaStreamTrack* track)
{
    return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *track }).ptr();
}

}
