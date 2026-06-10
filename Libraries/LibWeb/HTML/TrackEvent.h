/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/TrackEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using NullableTrackType = Variant<GC::Ref<VideoTrack>, GC::Ref<AudioTrack>, GC::Ref<TextTrack>, Empty>;

using TrackEventInit = Bindings::TrackEventInit;

class TrackEvent : public DOM::Event {
    WEB_WRAPPABLE(TrackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(TrackEvent);

public:
    [[nodiscard]] static GC::Ref<TrackEvent> create(FlyString const& event_name, TrackEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    // https://html.spec.whatwg.org/multipage/media.html#dom-trackevent-track
    NullableTrackType track() const;

private:
    TrackEvent(FlyString const& event_name, TrackEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Visitor&) override;

    NullableTrackType m_track;
};

}
