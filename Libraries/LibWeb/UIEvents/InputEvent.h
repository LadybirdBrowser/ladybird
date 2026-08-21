/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/InputEvent.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

using InputEventInit = Bindings::InputEventInit;

class InputEvent final : public UIEvent {
    WEB_WRAPPABLE(InputEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(InputEvent);

public:
    [[nodiscard]] static GC::Ref<InputEvent> create(FlyString const& type, InputEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<InputEvent> create(Utf16String const& type, InputEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<InputEvent> create(FlyString const& type, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<InputEvent> create_from_platform_event(FlyString const& type, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges = {}, HighResolutionTime::DOMHighResTimeStamp = 0);
    [[nodiscard]] static GC::Ref<InputEvent> create_from_platform_event(Utf16FlyString const& type, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges = {}, HighResolutionTime::DOMHighResTimeStamp = 0);
    [[nodiscard]] static GC::Ref<InputEvent> create(Utf16FlyString const& type, InputEventInit const&);

    virtual ~InputEvent() override;

    // https://w3c.github.io/uievents/#dom-inputevent-data
    Optional<Utf16String> data() const { return m_data; }

    // https://w3c.github.io/uievents/#dom-inputevent-iscomposing
    bool is_composing() const { return m_is_composing; }

    // https://w3c.github.io/uievents/#dom-inputevent-inputtype
    Utf16FlyString input_type() const { return m_input_type; }

    GC::Ptr<HTML::DataTransfer> data_transfer() const { return m_data_transfer; }
    static constexpr size_t data_transfer_offset() { return offsetof(InputEvent, m_data_transfer); }

    ReadonlySpan<GC::Ref<DOM::StaticRange>> get_target_ranges() const;

private:
    InputEvent(FlyString const& event_name, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges, HighResolutionTime::DOMHighResTimeStamp);
    InputEvent(Utf16FlyString const& event_name, InputEventInit const&, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(Visitor&) override;

    Optional<Utf16String> m_data;
    bool m_is_composing;
    Utf16FlyString m_input_type;
    GC::Ptr<HTML::DataTransfer> m_data_transfer;
    Vector<GC::Ref<DOM::StaticRange>> m_target_ranges;
};

}
