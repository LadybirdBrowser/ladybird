/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Timer.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class AutoScrollHandler {
public:
    AutoScrollHandler(HTML::Navigable&, DOM::Element& container);
    ~AutoScrollHandler();

    void visit_edges(JS::Cell::Visitor&) const;

    CSSPixelPoint process(CSSPixelPoint mouse_position);

    static GC::Ptr<DOM::Element> find_scrollable_ancestor(Painting::Paintable const&);

private:
    void start_timer();
    void stop_timer();
    void perform_tick();

    GC::Ref<HTML::Navigable> m_navigable;
    GC::Ref<DOM::Element> m_container_element;
    CSSPixelPoint m_mouse_position;
    CSSPixelPoint m_fractional_delta;
    RefPtr<Core::Timer> m_timer;
};

}
