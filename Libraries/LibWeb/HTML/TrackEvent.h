/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

using TrackType = Variant<GC::Ref<VideoTrack>, GC::Ref<AudioTrack>, GC::Ref<TextTrack>>;

// The Event of a new trusted TrackEvent in the realm, for the implementation to fire.
GC::Ref<DOM::Event> create_track_event(JS::Realm&, Utf16FlyString const& type, TrackType const& track);

}
