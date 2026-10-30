/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/WindowProxy.h>

namespace Web::SVG {

// https://svgwg.org/specs/animations/#InterfaceTimeEvent
class TimeEvent final : public DOM::Event {
    WEB_WRAPPABLE(TimeEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(TimeEvent);

public:
    [[nodiscard]] static GC::Ref<TimeEvent> create(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp);

    GC::Ptr<HTML::WindowProxy> view() const { return m_view; }
    i32 detail() const { return m_detail; }

    void init_time_event(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail);

private:
    TimeEvent(Utf16FlyString const& type, GC::Ptr<HTML::WindowProxy> view, i32 detail, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<HTML::WindowProxy> m_view;
    i32 m_detail { 0 };
};

}
