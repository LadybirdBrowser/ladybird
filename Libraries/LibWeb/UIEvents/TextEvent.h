/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class TextEvent final : public UIEvent {
    WEB_WRAPPABLE(TextEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(TextEvent);

public:
    [[nodiscard]] static GC::Ref<TextEvent> create(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~TextEvent() override;

    // https://w3c.github.io/uievents/#dom-textevent-data
    Utf16String const& data() const { return m_data; }

    void init_text_event(Utf16FlyString const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, Utf16String const& data);

private:
    TextEvent(Utf16FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp);

    Utf16String m_data;
};

}
