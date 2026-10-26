/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCPeerConnectionIceEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using RTCPeerConnectionIceEventInit = Bindings::RTCPeerConnectionIceEventInit;

class RTCPeerConnectionIceEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCPeerConnectionIceEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCPeerConnectionIceEvent);

public:
    [[nodiscard]] static GC::Ref<RTCPeerConnectionIceEvent> create(Utf16FlyString const& event_name, RTCPeerConnectionIceEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<RTCPeerConnectionIceEvent> create_for_constructor(Utf16String const& type, RTCPeerConnectionIceEventInit const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp) { return create(Utf16FlyString { type }, init, time_stamp); }
    GC::Ptr<RTCIceCandidate> candidate() const { return m_candidate; }
    Optional<Utf16String> url() const { return m_url; }
    virtual ~RTCPeerConnectionIceEvent() override;

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;
    GC::Ptr<RTCIceCandidate> m_candidate;
    Optional<Utf16String> m_url;
    RTCPeerConnectionIceEvent(Utf16FlyString const& event_name, RTCPeerConnectionIceEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
