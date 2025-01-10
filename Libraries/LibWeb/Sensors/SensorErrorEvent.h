/*
 * Copyright (c) 2025, Saksham Goyal <sakgoy2001@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::Sensors {

struct SensorErrorEventInit : public DOM::EventInit {
    GC::Ptr<WebIDL::DOMException> error;
};

// https://w3c.github.io/clipboard-apis/#clipboardevent
class SensorErrorEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(SensorErrorEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(SensorErrorEvent);

public:
    static GC::Ref<SensorErrorEvent> construct_impl(JS::Realm&, FlyString const& event_name, SensorErrorEventInit const& event_init);

    virtual ~SensorErrorEvent() override = default;

    auto const& error() const { return m_error; }

private:
    SensorErrorEvent(JS::Realm&, FlyString const& event_name, SensorErrorEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ptr<WebIDL::DOMException> m_error;
};

}
