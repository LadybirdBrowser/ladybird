/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/Bindings/CustomEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::DOM {

// https://dom.spec.whatwg.org/#customevent
class WEB_API CustomEvent : public Event {
    WEB_WRAPPABLE(CustomEvent, Event);
    GC_DECLARE_ALLOCATOR(CustomEvent);

public:
    [[nodiscard]] static GC::Ref<CustomEvent> create(JS::Object const& relevant_global_object, FlyString const& event_name, Bindings::CustomEventInit const& = {});
    [[nodiscard]] static GC::Ref<CustomEvent> create(FlyString const& event_name, Bindings::CustomEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    static WebIDL::ExceptionOr<GC::Ref<CustomEvent>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, FlyString const& event_name, Bindings::CustomEventInit const&);

    virtual ~CustomEvent() override;

    // https://dom.spec.whatwg.org/#dom-customevent-detail
    JS::Value detail() const { return m_detail; }

    virtual void visit_edges(GC::Cell::Visitor&) override;

    void init_custom_event(String const& type, bool bubbles, bool cancelable, JS::Value detail);

private:
    CustomEvent(FlyString const& event_name, Bindings::CustomEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);

    // https://dom.spec.whatwg.org/#dom-customevent-initcustomevent-type-bubbles-cancelable-detail-detail
    JS::Value m_detail { JS::js_null() };
};

}
