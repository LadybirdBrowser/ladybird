/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/CompositionEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

using CompositionEventInit = Bindings::CompositionEventInit;

class CompositionEvent final : public UIEvent {
    WEB_WRAPPABLE(CompositionEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(CompositionEvent);

public:
    [[nodiscard]] static GC::Ref<CompositionEvent> create(FlyString const& event_name, CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<CompositionEvent> create(Utf16String const& event_name, CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<CompositionEvent> create(Utf16FlyString const& event_name, CompositionEventInit const&);

    virtual ~CompositionEvent() override;

    // https://w3c.github.io/uievents/#dom-compositionevent-data
    Utf16String const& data() const { return m_data; }

    void init_composition_event(Utf16FlyString const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, Utf16String const& data);

private:
    CompositionEvent(FlyString const& event_name, CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    CompositionEvent(Utf16FlyString const& event_name, CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    Utf16String m_data;
};

}
