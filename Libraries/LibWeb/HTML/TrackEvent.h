/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibGC/Root.h>
#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

struct TrackEventInit : public DOM::EventInit {
    using TrackType = Optional<Variant<GC::Root<VideoTrack>, GC::Root<AudioTrack>, GC::Root<TextTrack>>>;
    TrackType track;
};

class TrackEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(TrackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(TrackEvent);

public:
    [[nodiscard]] static GC::Ref<TrackEvent> create(JS::Realm&, FlyString const& event_name, TrackEventInit = {});
    static WebIDL::ExceptionOr<GC::Ref<TrackEvent>> construct_impl(JS::Realm&, FlyString const& event_name, TrackEventInit);

    // https://html.spec.whatwg.org/multipage/media.html#dom-trackevent-track
    Variant<Empty, GC::Root<VideoTrack>, GC::Root<AudioTrack>, GC::Root<TextTrack>> track() const;

private:
    TrackEvent(JS::Realm&, FlyString const& event_name, TrackEventInit event_init);

    virtual void initialize(JS::Realm&) override;

    TrackEventInit::TrackType m_track;
};

}
