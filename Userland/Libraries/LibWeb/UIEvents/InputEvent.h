/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

struct InputEventInit : public UIEventInit {
    Optional<String> data;
    bool is_composing { false };
    FlyString input_type {};
};

class InputEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(InputEvent, UIEvent);
    JS_DECLARE_ALLOCATOR(InputEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<InputEvent> create_from_platform_event(JS::Realm&, FlyString const& type, InputEventInit const& event_init);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<InputEvent>> construct_impl(JS::Realm&, FlyString const& event_name, InputEventInit const& event_init);

    virtual ~InputEvent() override;

    // https://w3c.github.io/uievents/#dom-inputevent-data
    Optional<String> data() const { return m_data; }

    // https://w3c.github.io/uievents/#dom-inputevent-iscomposing
    bool is_composing() const { return m_is_composing; }

    // https://w3c.github.io/uievents/#dom-inputevent-inputtype
    FlyString input_type() const { return m_input_type; }

    Vector<DOM::StaticRange> get_target_ranges() const;

private:
    InputEvent(JS::Realm&, FlyString const& event_name, InputEventInit const&);

    virtual void initialize(JS::Realm&) override;

    Optional<String> m_data;
    bool m_is_composing;
    FlyString m_input_type;
};

}
