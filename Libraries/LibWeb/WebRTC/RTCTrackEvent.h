/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCTrackEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::WebRTC {

class RTCRtpReceiver;
class RTCRtpTransceiver;

using RTCTrackEventInit = Bindings::RTCTrackEventInit;

class RTCTrackEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCTrackEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCTrackEvent);

public:
    [[nodiscard]] static GC::Ref<RTCTrackEvent> create(Utf16FlyString const& event_name,
        GC::Ref<RTCRtpReceiver>, GC::Ref<MediaCapture::MediaStreamTrack>,
        Vector<GC::Ref<MediaCapture::MediaStream>>&& streams,
        GC::Ref<RTCRtpTransceiver>,
        HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<RTCTrackEvent>> construct_impl(Utf16String const& type, RTCTrackEventInit const&);
    virtual ~RTCTrackEvent() override;

    GC::Ref<RTCRtpReceiver> receiver() const;
    GC::Ref<RTCRtpTransceiver> transceiver() const;
    GC::Ref<MediaCapture::MediaStreamTrack> track() const;
    Vector<GC::Ref<MediaCapture::MediaStream>> const& streams() const { return m_streams; }

private:
    RTCTrackEvent(Utf16FlyString const& event_name,
        GC::Ref<RTCRtpReceiver>, GC::Ref<MediaCapture::MediaStreamTrack>,
        Vector<GC::Ref<MediaCapture::MediaStream>>&& streams,
        GC::Ref<RTCRtpTransceiver>,
        HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<RTCRtpReceiver> m_receiver;
    GC::Ref<MediaCapture::MediaStreamTrack> m_track;
    Vector<GC::Ref<MediaCapture::MediaStream>> m_streams;
    GC::Ref<RTCRtpTransceiver> m_transceiver;
};

}
