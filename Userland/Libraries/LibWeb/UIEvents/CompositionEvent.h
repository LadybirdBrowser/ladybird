/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

struct CompositionEventInit : public UIEventInit {
    String data;
};

class CompositionEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(CompositionEvent, UIEvent);
    JS_DECLARE_ALLOCATOR(CompositionEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CompositionEvent> create(JS::Realm&, FlyString const& event_name, CompositionEventInit const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CompositionEvent>> construct_impl(JS::Realm&, FlyString const& event_name, CompositionEventInit const& event_init);

    virtual ~CompositionEvent() override;

    // https://w3c.github.io/uievents/#dom-compositionevent-data
    String data() const { return m_data; }

    void init_composition_event(String const& type, bool bubbles, bool cancelable, HTML::Window* view, String const& data);

private:
    CompositionEvent(JS::Realm&, FlyString const& event_name, CompositionEventInit const&);

    virtual void initialize(JS::Realm&) override;

    String m_data;
};

}
