/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/Bindings/TransitionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class Window;

}

namespace Web::CSS {

using TransitionEventInit = Bindings::TransitionEventInit;

class TransitionEvent final : public DOM::Event {
    WEB_WRAPPABLE(TransitionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(TransitionEvent);

public:
    [[nodiscard]] static GC::Ref<TransitionEvent> create(FlyString const& event_name, TransitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<TransitionEvent> create(FlyString const& event_name, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~TransitionEvent() override;

    String const& property_name() const { return m_property_name; }
    double elapsed_time() const { return m_elapsed_time; }
    String const& pseudo_element() const { return m_pseudo_element; }

private:
    TransitionEvent(FlyString const& event_name, TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    TransitionEvent(FlyString const& event_name, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp);

    String m_property_name {};
    double m_elapsed_time {};
    String m_pseudo_element {};
};

}
