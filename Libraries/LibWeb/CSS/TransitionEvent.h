/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
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
    [[nodiscard]] static GC::Ref<TransitionEvent> create(Utf16FlyString const& event_name, Bindings::TransitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<TransitionEvent> create(Utf16FlyString const& event_name, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp, GC::Ptr<CSSTransition> animation = nullptr);
    [[nodiscard]] static GC::Ref<TransitionEvent> create_for_constructor(Utf16FlyString const& event_name, Bindings::TransitionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~TransitionEvent() override;

    Utf16String const& property_name() const { return m_property_name; }
    double elapsed_time() const { return m_elapsed_time; }
    Utf16String const& pseudo_element() const { return m_pseudo_element; }
    GC::Ptr<CSSTransition> animation() const { return m_animation; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    TransitionEvent(Utf16FlyString const& event_name, Bindings::TransitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    TransitionEvent(Utf16FlyString const& event_name, String property_name, double elapsed_time, String pseudo_element, HighResolutionTime::DOMHighResTimeStamp, GC::Ptr<CSSTransition> animation);

    Utf16String m_property_name {};
    double m_elapsed_time {};
    Utf16String m_pseudo_element {};
    GC::Ptr<CSSTransition> m_animation;
};

}
