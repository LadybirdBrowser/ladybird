/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/RTCDataChannelEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using RTCDataChannelEventInit = Bindings::RTCDataChannelEventInit;

class RTCDataChannelEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCDataChannelEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCDataChannelEvent);

public:
    [[nodiscard]] static GC::Ref<RTCDataChannelEvent> create(Utf16FlyString const& event_name, RTCDataChannelEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static GC::Ref<RTCDataChannelEvent> create_for_constructor(Utf16String const& type, RTCDataChannelEventInit const& init, HighResolutionTime::DOMHighResTimeStamp time_stamp) { return create(Utf16FlyString { type }, init, time_stamp); }
    GC::Ref<RTCDataChannel> channel() const { return m_channel; }
    virtual ~RTCDataChannelEvent() override;

private:
    virtual void visit_edges(GC::Cell::Visitor&) override;
    GC::Ref<RTCDataChannel> m_channel;
    RTCDataChannelEvent(Utf16FlyString const& event_name, RTCDataChannelEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
