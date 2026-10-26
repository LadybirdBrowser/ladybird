/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCPeerConnectionIceErrorEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using RTCPeerConnectionIceErrorEventInit = Bindings::RTCPeerConnectionIceErrorEventInit;

class RTCPeerConnectionIceErrorEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCPeerConnectionIceErrorEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCPeerConnectionIceErrorEvent);

public:
    [[nodiscard]] static GC::Ref<RTCPeerConnectionIceErrorEvent> create(Utf16FlyString const& event_name, RTCPeerConnectionIceErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~RTCPeerConnectionIceErrorEvent() override;

private:
    RTCPeerConnectionIceErrorEvent(Utf16FlyString const& event_name, RTCPeerConnectionIceErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
