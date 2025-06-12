/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>

namespace Web::CSS {

struct TransitionEventInit : public DOM::EventInit {
    String property_name {};
    double elapsed_time = 0.0;
    String pseudo_element {};
};

class TransitionEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(TransitionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(TransitionEvent);

public:
    [[nodiscard]] static GC::Ref<TransitionEvent> create(JS::Realm&, FlyString const& event_name, TransitionEventInit const& = {});
    [[nodiscard]] static GC::Ref<TransitionEvent> construct_impl(JS::Realm&, FlyString const& event_name, TransitionEventInit const& = {});

    virtual ~TransitionEvent() override;

    String const& property_name() const { return m_property_name; }
    double elapsed_time() const { return m_elapsed_time; }
    String const& pseudo_element() const { return m_pseudo_element; }

private:
    TransitionEvent(JS::Realm&, FlyString const& event_name, TransitionEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;

    String m_property_name {};
    double m_elapsed_time {};
    String m_pseudo_element {};
};

}
