/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/DOM/Event.h>

namespace Web::WebRTC {

class KeyFrameRequestEvent final : public DOM::Event {
    WEB_WRAPPABLE(KeyFrameRequestEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(KeyFrameRequestEvent);

public:
    [[nodiscard]] static GC::Ref<KeyFrameRequestEvent> create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<KeyFrameRequestEvent> create_for_constructor(Utf16String const& type, Optional<Utf16String> const& rid, HighResolutionTime::DOMHighResTimeStamp);
    virtual ~KeyFrameRequestEvent() override;

private:
    KeyFrameRequestEvent(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);
};

}
