/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/String.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Utils.h>

namespace Web::HTML {

struct ToggleEventInit : public DOM::EventInit {
    String old_state;
    String new_state;
};

class ToggleEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(ToggleEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ToggleEvent);

public:
    [[nodiscard]] static GC::Ref<ToggleEvent> create(JS::Realm&, FlyString const& event_name, ToggleEventInit = {}, GC::Ptr<DOM::Element> source = {});
    static WebIDL::ExceptionOr<GC::Ref<ToggleEvent>> construct_impl(JS::Realm&, FlyString const& event_name, ToggleEventInit);

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-oldstate
    String const& old_state() const { return m_old_state; }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-newstate
    String const& new_state() const { return m_new_state; }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-toggleevent-source
    GC::Ptr<DOM::Element> source() const
    {
        // The source getter steps are to return the result of retargeting source against this's currentTarget.
        return as<DOM::Element>(retarget(m_source, current_target()));
    }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    ToggleEvent(JS::Realm&, FlyString const& event_name, ToggleEventInit event_init, GC::Ptr<DOM::Element> source);

    virtual void initialize(JS::Realm&) override;

    String m_old_state;
    String m_new_state;
    GC::Ptr<DOM::Element> m_source;
};

}
