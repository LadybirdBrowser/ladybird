/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCErrorEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using RTCErrorEventInit = Bindings::RTCErrorEventInit;

class RTCErrorEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCErrorEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCErrorEvent);

public:
    [[nodiscard]] static GC::Ref<RTCErrorEvent> create(Utf16FlyString const& event_name, RTCErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~RTCErrorEvent() override;

private:
    RTCErrorEvent(Utf16FlyString const& event_name, RTCErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
