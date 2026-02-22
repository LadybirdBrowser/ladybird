/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>

namespace Web::MediaCapture {

// https://w3c.github.io/mediacapture-main/#dictdef-mediastreamtrackeventinit
struct MediaStreamTrackEventInit final : public DOM::EventInit {
    GC::Ptr<MediaStreamTrack> track;
};

// https://w3c.github.io/mediacapture-main/#mediastreamtrackevent
class MediaStreamTrackEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(MediaStreamTrackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(MediaStreamTrackEvent);

public:
    [[nodiscard]] static GC::Ref<MediaStreamTrackEvent> create(JS::Realm&, FlyString const& event_name, MediaStreamTrackEventInit const& event_init);
    [[nodiscard]] static GC::Ref<MediaStreamTrackEvent> construct_impl(JS::Realm&, FlyString const& event_name, MediaStreamTrackEventInit const& event_init);

    virtual ~MediaStreamTrackEvent() override;

    GC::Ref<MediaStreamTrack> track() const { return m_track; }

private:
    MediaStreamTrackEvent(JS::Realm&, FlyString const& event_name, MediaStreamTrackEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<MediaStreamTrack> m_track;
};

}
