/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/AudioTrack.h>
#include <LibWeb/HTML/TextTrack.h>
#include <LibWeb/HTML/TrackEvent.h>
#include <LibWeb/HTML/VideoTrack.h>

namespace Web::HTML {

extern "C" {
// Defined in Bindings/Rust/src/html/track_event.rs.
DOM::Event* web_rust_track_event_create(JSVM*, void* realm, FlatPtr type, GC::Cell* track);
}

GC::Ref<DOM::Event> create_track_event(JS::Realm& realm, Utf16FlyString const& type, TrackType const& track)
{
    auto* track_cell = track.visit([](auto const& track) -> GC::Cell* { return track.ptr(); });
    return *web_rust_track_event_create(JS::HostABI::vm_to_abi(realm.vm()), &realm, type.raw_identity(), track_cell);
}

}
