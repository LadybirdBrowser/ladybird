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
    Optional<Utf16String> data;
    bool is_composing { false };
    FlyString input_type {};
};

class InputEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(InputEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(InputEvent);

public:
    [[nodiscard]] static GC::Ref<InputEvent> create_from_platform_event(JS::Realm&, FlyString const& type, InputEventInit const& event_init, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges = {});
    static WebIDL::ExceptionOr<GC::Ref<InputEvent>> construct_impl(JS::Realm&, FlyString const& event_name, InputEventInit const& event_init);

    virtual ~InputEvent() override;

    // https://w3c.github.io/uievents/#dom-inputevent-data
    Optional<Utf16String> data() const { return m_data; }

    // https://w3c.github.io/uievents/#dom-inputevent-iscomposing
    bool is_composing() const { return m_is_composing; }

    // https://w3c.github.io/uievents/#dom-inputevent-inputtype
    FlyString input_type() const { return m_input_type; }

    ReadonlySpan<GC::Ref<DOM::StaticRange>> get_target_ranges() const;

private:
    InputEvent(JS::Realm&, FlyString const& event_name, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    Optional<Utf16String> m_data;
    bool m_is_composing;
    FlyString m_input_type;
    Vector<GC::Ref<DOM::StaticRange>> m_target_ranges;
};

}
