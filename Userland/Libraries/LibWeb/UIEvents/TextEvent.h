/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class TextEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(TextEvent, UIEvent);
    JS_DECLARE_ALLOCATOR(TextEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<TextEvent> create(JS::Realm&, FlyString const& event_name);

    virtual ~TextEvent() override;

    // https://w3c.github.io/uievents/#dom-textevent-data
    String data() const { return m_data; }

    void init_text_event(String const& type, bool bubbles, bool cancelable, HTML::Window* view, String const& data);

private:
    TextEvent(JS::Realm&, FlyString const& event_name);

    virtual void initialize(JS::Realm&) override;

    String m_data;
};

}
