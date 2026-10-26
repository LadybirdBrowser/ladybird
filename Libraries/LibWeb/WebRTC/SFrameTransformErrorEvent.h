/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/SFrameTransformErrorEvent.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

using SFrameTransformErrorEventInit = Bindings::SFrameTransformErrorEventInit;

class SFrameTransformErrorEvent final : public DOM::Event {
    WEB_WRAPPABLE(SFrameTransformErrorEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SFrameTransformErrorEvent);

public:
    [[nodiscard]] static GC::Ref<SFrameTransformErrorEvent> create(Utf16FlyString const& event_name, SFrameTransformErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~SFrameTransformErrorEvent() override;

private:
    SFrameTransformErrorEvent(Utf16FlyString const& event_name, SFrameTransformErrorEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
