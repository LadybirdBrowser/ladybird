/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCDTMFToneChangeEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using RTCDTMFToneChangeEventInit = Bindings::RTCDTMFToneChangeEventInit;

class RTCDTMFToneChangeEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCDTMFToneChangeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCDTMFToneChangeEvent);

public:
    [[nodiscard]] static GC::Ref<RTCDTMFToneChangeEvent> create(Utf16FlyString const& event_name, RTCDTMFToneChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~RTCDTMFToneChangeEvent() override;

private:
    RTCDTMFToneChangeEvent(Utf16FlyString const& event_name, RTCDTMFToneChangeEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
