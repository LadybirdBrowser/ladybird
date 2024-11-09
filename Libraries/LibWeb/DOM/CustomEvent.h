/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>

namespace Web::DOM {

struct CustomEventInit : public EventInit {
    JS::Value detail { JS::js_null() };
};

// https://dom.spec.whatwg.org/#customevent
class CustomEvent : public Event {
    WEB_PLATFORM_OBJECT(CustomEvent, Event);
    JS_DECLARE_ALLOCATOR(CustomEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CustomEvent> create(JS::Realm&, FlyString const& event_name, CustomEventInit const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<CustomEvent>> construct_impl(JS::Realm&, FlyString const& event_name, CustomEventInit const&);

    virtual ~CustomEvent() override;

    // https://dom.spec.whatwg.org/#dom-customevent-detail
    JS::Value detail() const { return m_detail; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    void init_custom_event(String const& type, bool bubbles, bool cancelable, JS::Value detail);

private:
    CustomEvent(JS::Realm&, FlyString const& event_name, CustomEventInit const& event_init);

    // https://dom.spec.whatwg.org/#dom-customevent-initcustomevent-type-bubbles-cancelable-detail-detail
    JS::Value m_detail { JS::js_null() };
};

}
