/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PopStateEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

using PopStateEventInit = Bindings::PopStateEventInit;

class PopStateEvent final : public DOM::Event {
    WEB_WRAPPABLE(PopStateEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(PopStateEvent);

public:
    [[nodiscard]] static GC::Ref<PopStateEvent> create(FlyString const& event_name, PopStateEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
    [[nodiscard]] static GC::Ref<PopStateEvent> create(FlyString const& event_name, JS::Value state, HighResolutionTime::DOMHighResTimeStamp);

    JS::Value const& state() const { return m_state; }

private:
    PopStateEvent(FlyString const& event_name, PopStateEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp);
    PopStateEvent(FlyString const& event_name, JS::Value state, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor& visitor) override;

    JS::Value m_state { JS::js_null() };
};

}
