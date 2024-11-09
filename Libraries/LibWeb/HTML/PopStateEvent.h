/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>

namespace Web::HTML {

struct PopStateEventInit : public DOM::EventInit {
    JS::Value state { JS::js_null() };
};

class PopStateEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(PopStateEvent, DOM::Event);
    JS_DECLARE_ALLOCATOR(PopStateEvent);

public:
    [[nodiscard]] static JS::NonnullGCPtr<PopStateEvent> create(JS::Realm&, FlyString const& event_name, PopStateEventInit const&);
    [[nodiscard]] static JS::NonnullGCPtr<PopStateEvent> construct_impl(JS::Realm&, FlyString const& event_name, PopStateEventInit const&);

    JS::Value const& state() const { return m_state; }

private:
    PopStateEvent(JS::Realm&, FlyString const& event_name, PopStateEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    JS::Value m_state { JS::js_null() };
};

}
