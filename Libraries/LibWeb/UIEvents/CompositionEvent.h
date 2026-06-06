/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/CompositionEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class CompositionEvent final : public UIEvent {
    WEB_WRAPPABLE(CompositionEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(CompositionEvent);

public:
    [[nodiscard]] static GC::Ref<CompositionEvent> create(FlyString const& event_name, Bindings::CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<CompositionEvent>> construct_impl(HTML::Window&, FlyString const& event_name, Bindings::CompositionEventInit const& event_init);

    virtual ~CompositionEvent() override;

    // https://w3c.github.io/uievents/#dom-compositionevent-data
    String data() const { return m_data; }

    void init_composition_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, String const& data);

private:
    CompositionEvent(FlyString const& event_name, Bindings::CompositionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    String m_data;
};

}
