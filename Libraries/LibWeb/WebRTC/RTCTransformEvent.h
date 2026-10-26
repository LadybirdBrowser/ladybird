/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::WebRTC {

class RTCRtpScriptTransformer;

class WEB_API RTCTransformEvent final : public DOM::Event {
    WEB_WRAPPABLE(RTCTransformEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(RTCTransformEvent);

public:
    static GC::Ref<RTCTransformEvent> create(Utf16FlyString const& event_name, GC::Ref<RTCRtpScriptTransformer>, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~RTCTransformEvent() override;

    GC::Ref<RTCRtpScriptTransformer> transformer() const;

private:
    RTCTransformEvent(Utf16FlyString const&, GC::Ref<RTCRtpScriptTransformer>, HighResolutionTime::DOMHighResTimeStamp);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<RTCRtpScriptTransformer> m_transformer;
};

}
