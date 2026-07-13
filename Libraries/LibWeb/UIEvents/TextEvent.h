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
    WEB_PLATFORM_OBJECT(TextEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(TextEvent);

public:
    [[nodiscard]] static GC::Ref<TextEvent> create(JS::Realm&, Utf16FlyString const& event_name);

    virtual ~TextEvent() override;

    // https://w3c.github.io/uievents/#dom-textevent-data
    Utf16String const& data() const { return m_data; }

    void init_text_event(Utf16FlyString const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, Utf16String const& data);

private:
    TextEvent(JS::Realm&, Utf16FlyString const& event_name);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_data;
};

}
