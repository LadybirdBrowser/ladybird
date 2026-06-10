/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MediaStreamTrackEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>

namespace Web::HTML {

class Window;

}

namespace Web::MediaCapture {

using MediaStreamTrackEventInit = Bindings::MediaStreamTrackEventInit;

// https://w3c.github.io/mediacapture-main/#mediastreamtrackevent
class MediaStreamTrackEvent final : public DOM::Event {
    WEB_WRAPPABLE(MediaStreamTrackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MediaStreamTrackEvent);

public:
    [[nodiscard]] static GC::Ref<MediaStreamTrackEvent> create(FlyString const& event_name, MediaStreamTrackEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~MediaStreamTrackEvent() override;

    GC::Ref<MediaStreamTrack> track() const { return m_track; }

private:
    MediaStreamTrackEvent(FlyString const& event_name, MediaStreamTrackEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<MediaStreamTrack> m_track;
};

}
