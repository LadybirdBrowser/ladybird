/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/CompositionEvent.h>
#include <LibWeb/UIEvents/UIEvent.h>

namespace Web::UIEvents {

class CompositionEvent final : public UIEvent {
    WEB_PLATFORM_OBJECT(CompositionEvent, UIEvent);
    GC_DECLARE_ALLOCATOR(CompositionEvent);

public:
    [[nodiscard]] static GC::Ref<CompositionEvent> create(JS::Realm&, Utf16FlyString const& event_name, Bindings::CompositionEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<CompositionEvent>> construct_impl(JS::Realm&, Utf16FlyString const& event_name, Bindings::CompositionEventInit const& event_init);

    virtual ~CompositionEvent() override;

    // https://w3c.github.io/uievents/#dom-compositionevent-data
    Utf16String const& data() const { return m_data; }

    void init_composition_event(Utf16FlyString const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, Utf16String const& data);

private:
    CompositionEvent(JS::Realm&, Utf16FlyString const& event_name, Bindings::CompositionEventInit const&);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_data;
};

}
